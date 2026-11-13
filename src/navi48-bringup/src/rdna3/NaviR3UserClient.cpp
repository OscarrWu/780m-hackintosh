// NaviR3UserClient.cpp - selector 0 reads the assembled survey text as offset
// chunks (scalar in offset / struct out capped at NAVI48_UC_MAX_XFER / scalar
// out total); selector 1 triggers the stage-2 power-up substeps (D-01, root only).
#include "NaviR3UserClient.hpp"
#include "NaviR3Bringup.hpp"

#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include "Navi48UserClientABI.h"   // kNavi48SelReadLog, LogReset, LogConsume — single-point definition
#include "n48log.h"


OSDefineMetaClassAndStructors(NaviR3UserClient, IOUserClient)

bool NaviR3UserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
                                    OSDictionary *properties) {
    if (!super::initWithTask(owningTask, securityID, type, properties)) return false;
    // The survey reads GPU registers; keep the interface root-only.
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

// 0.0.276: log primitives. Argument layouts match Navi48UserClient.cpp exactly.

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

IOReturn NaviR3UserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                          IOExternalMethodDispatch *dispatch, OSObject *target,
                                          void *reference) {
    switch (selector) {
    case kN48R3SelTrigger:
        // D-01: trigger the five power-up substeps. The admin gate already ran in
        // initWithTask; triggerPowerUp checks the D-04 one-boot marker internally.
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
        // Chunked read of the assembled survey text, same shape as the base
        // doReadLog: scalar in offset, struct out capped at NAVI48_UC_MAX_XFER,
        // scalar out total. Inband memcpy only; no OOL descriptor is involved.
        const uint32_t offset = (uint32_t)args->scalarInput[0];
        uint32_t max = (uint32_t)args->structureOutputSize;
        if (max > NAVI48_UC_MAX_XFER) max = NAVI48_UC_MAX_XFER;
        const char *text = owner_->surveyText();
        size_t n = 0;
        while (n < R3_SURVEY_TEXT_BYTES && text[n]) n++;
        if (offset >= n) {
            // Past-the-end read: clamp to an empty chunk and report the total,
            // matching the base log-read clamp behaviour.
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
    default:
        return super::externalMethod(selector, args, dispatch, target, reference);
    }
}
