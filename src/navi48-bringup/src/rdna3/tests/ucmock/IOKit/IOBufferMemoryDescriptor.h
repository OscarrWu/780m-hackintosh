// IOBufferMemoryDescriptor stub for host tests (ucmock).
// Real IOKit header; this provides enough for NaviR3UserClient.cpp to compile.
// OSObject etc. are defined in IOUserClient.h (included first by the test file).
#ifndef _IOKIT_IOBUFFERMEMORYDESCRIPTOR_H
#define _IOKIT_IOBUFFERMEMORYDESCRIPTOR_H

#include <cstdint>

enum {
    kIOMemoryKernelUserShared = 0x00000004u,
};

class IOBufferMemoryDescriptor : public OSObject {
public:
    static IOBufferMemoryDescriptor *inTaskWithOptions(
        void *task, uint32_t options, uint64_t capacity, uint32_t alignment = 1)
    { (void)task; (void)options; (void)capacity; (void)alignment; return nullptr; }

    void *getBytesNoCopy() { return nullptr; }
    uint64_t getLength() const { return 0; }
    uint64_t getCapacity() const { return 0; }
};

#endif
