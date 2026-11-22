// NaviR3UserClient.cpp - stage-3 ABI: selector 0 (survey info), 1 (trigger
// power-up), 2-5 (BO alloc/free, IB submit, fence read). Selector numbers and
// call shapes come from the contract header Navi48R3ABI.h (C-10); selectors
// 21/100-113 are registered but return kIOReturnUnsupported.
//
#include "NaviR3UserClient.hpp"
#include "NaviR3Bringup.hpp"

#include <IOKit/IOLib.h>
#ifndef N48R3_HOST_TEST
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/libkern.h>
#endif
#include "Navi48UserClientABI.h"
#include "n48log.h"



OSDefineMetaClassAndStructors(NaviR3UserClient, IOUserClient)

bool NaviR3UserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
                                    OSDictionary *properties) {
    if (!super::initWithTask(owningTask, securityID, type, properties)) return false;
    if (clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
        IOLog("N48R3LOG userclient: open refused (caller not privileged)\n");
        return false;
    }
    return true;
}

bool NaviR3UserClient::start(IOService *provider) {
    owner_ = OSDynamicCast(NaviR3Bringup, provider);
    if (!owner_) return false;
    if (!super::start(provider)) return false;
    opened_ = true;
    return true;
}

void NaviR3UserClient::stop(IOService *provider) {
    opened_ = false;
    owner_ = nullptr;
    super::stop(provider);
}

IOReturn NaviR3UserClient::clientClose() {
    if (opened_) terminate();
    return kIOReturnSuccess;
}

// ---- log primitives (0.0.276) ----

IOReturn NaviR3UserClient::doReadLog(IOExternalMethodArguments *args) {
    if (!args || args->scalarInputCount < 1 || !args->structureOutput) return kIOReturnBadArgument;
    if (args->scalarOutputCount < 1) return kIOReturnBadArgument;
    const uint32_t offset = (uint32_t)args->scalarInput[0];
    uint32_t max = (uint32_t)args->structureOutputSize;
    if (max > NAVI48_UC_MAX_XFER) max = NAVI48_UC_MAX_XFER;
    uint32_t total = 0;
    const uint32_t got = amdgpu::n48_log_read(offset, args->structureOutput, max, &total);
    args->structureOutputSize = got;
    args->scalarOutput[0] = total;
    return kIOReturnSuccess;
}

IOReturn NaviR3UserClient::doLogReset(IOExternalMethodArguments *args) {
    (void)args;
    amdgpu::n48_log_reset();
    return kIOReturnSuccess;
}

IOReturn NaviR3UserClient::doLogConsume(IOExternalMethodArguments *args) {
    if (!args || args->scalarInputCount < 1 || args->scalarOutputCount < 4) return kIOReturnBadArgument;
    uint64_t consumed = 0, dropped = 0;
    const uint32_t held = amdgpu::n48_log_consume((uint32_t)args->scalarInput[0], &consumed, &dropped);
    args->scalarOutput[0] = held;
    args->scalarOutput[1] = consumed;
    args->scalarOutput[2] = dropped;
    args->scalarOutput[3] = amdgpu::n48_log_overflowed() ? 1u : 0u;
    return kIOReturnSuccess;
}

// ---- selector 2: BO alloc (domain + size -> gpu_va + handle; C-10/A-07) ----
#ifndef N48R3_HOST_TEST
IOReturn NaviR3UserClient::doBOAlloc(IOExternalMethodArguments *args) {
    if (!args || args->scalarInputCount < 2 || args->scalarOutputCount < 3)
        return kIOReturnBadArgument;
    if (!owner_) return kIOReturnNotReady;
    const uint64_t size   = args->scalarInput[0];
    const uint64_t domain = args->scalarInput[1];
    if (n48r3_bo_alloc_args(size, domain) != 0)
        return kIOReturnBadArgument;
    uint64_t gpu_va = 0, handle = 0;
    IOReturn kr = owner_->r3AllocBO(static_cast<uint32_t>(domain), size, &gpu_va, &handle);
    if (kr != kIOReturnSuccess) return kr;
    args->scalarOutput[0] = static_cast<uint32_t>(gpu_va & 0xFFFFFFFFull);
    args->scalarOutput[1] = static_cast<uint32_t>(gpu_va >> 32);
    args->scalarOutput[2] = handle;
    return kIOReturnSuccess;
}
IOReturn NaviR3UserClient::doBOFree(IOExternalMethodArguments *args) {
    if (!args || args->scalarInputCount < 2) return kIOReturnBadArgument;
    if (!owner_) return kIOReturnNotReady;
    const uint64_t gpu_va = n48r3_va_from_scalars(args->scalarInput[0], args->scalarInput[1]);
    if (gpu_va == 0) return kIOReturnBadArgument;
    return owner_->r3FreeBO(gpu_va);
}
IOReturn NaviR3UserClient::doSubmit(IOExternalMethodArguments *args) {
    if (!args || args->structureInput == nullptr) return kIOReturnBadArgument;
    if (!owner_) return kIOReturnNotReady;
    const N48R3CsIn *header = nullptr;
    const N48R3CsIb *ibs = nullptr;
    const int rc = n48r3_cs_in_validate(args->structureInput,
                                        static_cast<uint32_t>(args->structureInputSize),
                                        &header, &ibs);
    if (rc != 0) return kIOReturnBadArgument;
    uint64_t fence = 0;
    IOReturn kr = owner_->r3SubmitIB(ibs, header->num_ibs, &fence);
    if (kr != kIOReturnSuccess) return kr;
    if (args->scalarOutputCount >= 1)
        args->scalarOutput[0] = static_cast<uint32_t>(fence & 0xFFFFFFFFull);
    if (args->scalarOutputCount >= 2)
        args->scalarOutput[1] = static_cast<uint32_t>(fence >> 32);
    return kIOReturnSuccess;
}
IOReturn NaviR3UserClient::doFenceRead(IOExternalMethodArguments *args) {
    if (!args || args->scalarInputCount < 1) return kIOReturnBadArgument;
    if (!owner_) return kIOReturnNotReady;
    const uint64_t expected_seq = args->scalarInput[0];
    const uint32_t timeout_us = (args->scalarInputCount >= 2)
        ? static_cast<uint32_t>(args->scalarInput[1]) : 2000000u;
    uint64_t observed = 0;
    bool timed_out = false;
    IOReturn kr = owner_->r3WaitFence(expected_seq, timeout_us, &observed, &timed_out);
    if (kr != kIOReturnSuccess) return kr;
    if (args->scalarOutputCount >= 1)
        args->scalarOutput[0] = static_cast<uint32_t>(observed & 0xFFFFFFFFull);
    if (args->scalarOutputCount >= 2)
        args->scalarOutput[1] = static_cast<uint32_t>(observed >> 32);
    if (args->scalarOutputCount >= 3)
        args->scalarOutput[2] = timed_out ? 1u : 0u;
    return kIOReturnSuccess;
}
// ---- CPU-side mapping: BO handles + D6 readonly fence page (A-07/C-04) ----
IOReturn NaviR3UserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                               IOMemoryDescriptor **memory) {
    if (memory == nullptr) return kIOReturnBadArgument;
    *memory = nullptr;
    if (options != nullptr) *options = 0;
    if (!owner_) return kIOReturnNotReady;
    if (type == kN48R3MemoryTypeFenceRo)
        *memory = owner_->r3FenceRoMemory();
    else if (type >= 1u && type <= N48R3_BO_MAX)
        *memory = owner_->r3BoMemory(type, options);  // handle == kernel map type
    else
        return super::clientMemoryForType(type, options, memory);
    return *memory != nullptr ? kIOReturnSuccess : kIOReturnNotFound;
}
#endif /* N48R3_HOST_TEST */


// ---- main dispatch ----
IOReturn NaviR3UserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                          IOExternalMethodDispatch *dispatch, OSObject *target,
                                          void *reference) {
    switch (selector) {
    case kN48R3SelTrigger:
        if (!owner_) return kIOReturnNotReady;
        return owner_->triggerPowerUp();
    case kNavi48SelReadLog:
        return doReadLog(args);
    case kNavi48SelLogReset:
        return doLogReset(args);
    case kNavi48SelLogConsume:
        return doLogConsume(args);
    case kN48R3SelInfo: {
        if (!owner_ || !args) return kIOReturnNotReady;
        if (args->scalarInputCount < 1 || !args->structureOutput || !args->structureOutputSize
            || args->scalarOutputCount < 1) return kIOReturnBadArgument;
        const uint32_t offset = (uint32_t)args->scalarInput[0];
        uint32_t max = (uint32_t)args->structureOutputSize;
        if (max > NAVI48_UC_MAX_XFER) max = NAVI48_UC_MAX_XFER;
        const char *text = owner_->surveyText();
        size_t n = 0;
        while (n < R3_SURVEY_TEXT_BYTES && text[n]) n++;
        if (offset >= n) {
            args->structureOutputSize = 0;
            args->scalarOutput[0] = n;
            return kIOReturnSuccess;
        }
        const size_t remain = n - offset;
        const size_t take = remain < max ? remain : max;
        memcpy(args->structureOutput, text + offset, take);
        args->structureOutputSize = take;
        args->scalarOutput[0] = n;
        return kIOReturnSuccess;
    }
#ifndef N48R3_HOST_TEST
    case kN48R3SelBoAlloc:   return doBOAlloc(args);
    case kN48R3SelBoFree:    return doBOFree(args);
    case kN48R3SelSubmit:    return doSubmit(args);
    case kN48R3SelFenceRead: return doFenceRead(args);
#endif

    // 6-8: reserved (return unsupported)
    case 6: case 7: case 8:
        return kIOReturnUnsupported;

    // 21: BOIMPORTHOST — registered but not implemented (phase-5 feature)
    case kN48R3SelBoImportHost:
        return kIOReturnUnsupported;

    // 100-113: DBG_* — registered but not implemented
    default:
        if (selector >= kN48R3SelDbgSmu && selector <= kN48R3SelDbgClaim)
            return kIOReturnUnsupported;
        return super::externalMethod(selector, args, dispatch, target, reference);
    }
}
