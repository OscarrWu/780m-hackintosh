// ucmock/IOKit/IOUserClient.h - host stand-in for the IOKit user-client
// surface NaviR3UserClient.cpp uses, so its selector dispatch can be
// unit-tested on Linux (the real IOKit headers are Apple-only).
//
// The struct field set mirrors xnu-8796 (macOS 13) iokit/IOKit/IOUserClient.h;
// the MIG-stub call shapes this mock exists to model are documented in
// tmp/general/s3-ool-rootcause.md: on OOL calls the kernel stub still passes
// the 4096 B reply inband buffer (structureOutput non-NULL) with
// structureOutputSize == 0, plus the OOL descriptor.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOReturn.h>   // dalshim: the real kIOReturn values

typedef void    *task_t;
typedef uint32_t UInt32;
typedef uint64_t IOByteCount;

enum { kIOClientPrivilegeAdministrator = 1 };

class OSObject {
public:
    virtual ~OSObject() {}
};

class OSDictionary : public OSObject {};

class IOMemoryDescriptor : public OSObject {
public:
    virtual IOByteCount writeBytes(IOByteCount offset, const void *bytes,
                                   IOByteCount length) = 0;
};

// Field order follows xnu; the driver only touches the scalar and
// structure-output members.
struct IOExternalMethodArguments {
    uint32_t            selector;
    uint64_t           *scalarInput;
    uint32_t            scalarInputCount;
    uint64_t           *scalarOutput;
    uint32_t            scalarOutputCount;
    void               *structureInput;
    uint32_t            structureInputSize;
    IOMemoryDescriptor *structureInputDescriptor;
    void               *structureOutput;
    uint32_t            structureOutputSize;
    IOMemoryDescriptor *structureOutputDescriptor;
    uint32_t            structureOutputDescriptorSize;
};

typedef IOReturn (*IOExternalMethodAction)(OSObject *target, void *reference,
                                           IOExternalMethodArguments *arguments);
struct IOExternalMethodDispatch {
    IOExternalMethodAction function;
    uint32_t checkScalarInputCount;
    uint32_t checkStructureInputSize;
    uint32_t checkScalarOutputCount;
    uint32_t checkStructureOutputSize;
};

class IOService : public OSObject {
public:
    virtual bool start(IOService *provider) { (void)provider; return true; }
    virtual void stop(IOService *provider)  { (void)provider; }
    void terminate() {}
};

class IOUserClient : public IOService {
public:
    virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
                              OSDictionary *properties) {
        (void)owningTask; (void)securityID; (void)type; (void)properties;
        return true;
    }
    virtual IOReturn clientClose() { return kIOReturnSuccess; }
    virtual IOReturn externalMethod(uint32_t selector,
                                    IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch,
                                    OSObject *target, void *reference) {
        (void)selector; (void)args; (void)dispatch; (void)target; (void)reference;
        return kIOReturnUnsupported;
    }
};

static inline IOReturn clientHasPrivilege(void *securityID, uint32_t privilege) {
    (void)securityID; (void)privilege;
    return kIOReturnSuccess;
}

#define OSDeclareDefaultStructors(name) \
public: \
    name(); \
    virtual ~name();

#define OSDefineMetaClassAndStructors(CLASS, SUPER) \
    CLASS::CLASS() : SUPER() {} \
    CLASS::~CLASS() {}

#define OSDynamicCast(type, inst) dynamic_cast<type*>(inst)
