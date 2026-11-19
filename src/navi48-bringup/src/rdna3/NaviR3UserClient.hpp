// NaviR3UserClient.hpp - the root-only user client for the stage-3 ABI:
// selector 0 (survey info), 1 (trigger power-up), 2-5 (BO alloc/free, IB
// submit, fence read) plus BO memory mapping for CPU access (clientMemoryForType).
// Selector numbers/structs come from the contract header Navi48R3ABI.h so that
// kext and the n48r3test tool share one definition (C-10).
#ifndef N48_RDNA3_USERCLIENT_HPP
#define N48_RDNA3_USERCLIENT_HPP

#include <IOKit/IOUserClient.h>
#include "../Navi48R3ABI.h"

class NaviR3Bringup;

class NaviR3UserClient : public IOUserClient {
    OSDeclareDefaultStructors(NaviR3UserClient)
    using super = IOUserClient;

public:
    bool     initWithTask(task_t owningTask, void *securityID, UInt32 type,
                          OSDictionary *properties) override;
    bool     start(IOService *provider) override;
    void     stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override;
#ifndef N48R3_HOST_TEST
    // C-10/A-07: CPU access to BO backing (UMA: BAR0 window slice; GTT: sysmem
    // pages; special type kN48R3MemoryTypeFenceRo: D6 readonly fence page).
    // The kernel map type matches the handle returned by selector 2 (1..N48R3_BO_MAX),
    // same pattern as Navi48UserClient.cpp:31-37 / Navi48NativeClient.cpp:131.
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                 IOMemoryDescriptor **memory) override;
#endif
private:
    // 0.0.276: log primitives, selector numbers from Navi48UserClientABI.h (included in .cpp).
    IOReturn doReadLog(IOExternalMethodArguments *args);
    IOReturn doLogReset(IOExternalMethodArguments *args);
    IOReturn doLogConsume(IOExternalMethodArguments *args);
    /* T-E: ABI selectors 2-5 — BO alloc/free, IB submit, fence read */
    IOReturn doBOAlloc(IOExternalMethodArguments *args);
    IOReturn doBOFree(IOExternalMethodArguments *args);
    IOReturn doSubmit(IOExternalMethodArguments *args);
    IOReturn doFenceRead(IOExternalMethodArguments *args);

    NaviR3Bringup *owner_ { nullptr };
    bool           opened_ { false };
};

#endif // N48_RDNA3_USERCLIENT_HPP
