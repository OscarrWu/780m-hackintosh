// NaviR3UserClient.hpp - the root-only info client for the RDNA3 stage-1 survey.
#ifndef N48_RDNA3_USERCLIENT_HPP
#define N48_RDNA3_USERCLIENT_HPP

#include <IOKit/IOUserClient.h>

class NaviR3Bringup;

enum { kN48R3SelInfo = 0, kN48R3SelTrigger = 1 };   // selector 0: survey text; 1: trigger power-up

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

private:
    // 0.0.276: log primitives, selector numbers from Navi48UserClientABI.h (included in .cpp).
    IOReturn doReadLog(IOExternalMethodArguments *args);
    IOReturn doLogReset(IOExternalMethodArguments *args);
    IOReturn doLogConsume(IOExternalMethodArguments *args);

    NaviR3Bringup *owner_ { nullptr };
    bool           opened_ { false };
};

#endif // N48_RDNA3_USERCLIENT_HPP
