#ifndef GP108_BRIDGE_KEXT_HPP
#define GP108_BRIDGE_KEXT_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOSharedDataQueue.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>

#include "../include/GP108BridgeShared.h"

class com_maple_driver_GP108BridgeUserClient;

class com_maple_driver_GP108Bridge : public IOService {
    OSDeclareDefaultStructors(com_maple_driver_GP108Bridge)

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn newUserClient(task_t owningTask, void *securityID,
                                   UInt32 type, OSDictionary *properties,
                                   IOUserClient **handler) APPLE_KEXT_OVERRIDE;

    /* Hardware operations used by UserClient */
    void fillDeviceInfo(GP108DeviceInfo *outInfo);
    IOReturn readVBIOSChunk(uint32_t offset, void *dst, uint32_t *inOutSize);
    IOReturn pciConfigAccess(bool isWrite, bool targetBridge, uint32_t width,
                             uint32_t offset, uint32_t valueIn, uint32_t *valueOut);
    IOReturn powerControl(uint32_t action);
    IOReturn interruptControl(bool enable, uint64_t *outIrqCount, uint32_t *outLastIntr0);
    IOReturn readACPITableChunk(uint32_t sigU32, uint32_t instance,
                                uint32_t offset, void *dst, uint32_t *inOutSize);

    IODeviceMemory *getBarMemory(uint32_t barSlot);
    void setActiveEventQueue(IOSharedDataQueue *queue);

private:
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *sender, int count);

    bool locateACPIDevices();
    void savePCIConfig();
    void restorePCIConfig();
    void maskHardwareInterrupts();
    bool extractVBIOS();
    bool extractVBIOSFromACPI(IOACPIPlatformDevice *acpiDev);
    bool extractVBIOSFromBAR0();
    int32_t readDieTempCelsius();
    uint64_t readVRAMBytesStrap();

    IOLock                       *fLock;
    IOPCIDevice                  *fPCIDevice;
    IOPCIDevice                  *fBridgeDevice;
    IOACPIPlatformDevice         *fACPIDevice;
    IOACPIPlatformDevice         *fBridgeACPIDevice;

    IODeviceMemory               *fBarMemory[3];
    IOMemoryMap                  *fBar0Map;
    volatile uint32_t            *fBar0Virt;

    IOWorkLoop                   *fWorkLoop;
    IOFilterInterruptEventSource *fInterruptSource;
    int32_t                       fMsiIndex;
    bool                          fIrqEnabled;
    volatile uint64_t             fIrqCount;
    volatile uint32_t             fPendingIntr0;
    volatile uint32_t             fLastIntr0;
    IOSharedDataQueue            *fActiveQueue;

    bool                          fPoweredOn;
    uint32_t                      fSavedConfig[64];

    uint8_t                      *fVBIOSBuffer;
    uint32_t                      fVBIOSSize;
    uint32_t                      fVBIOSSource;
    char                          fACPIPath[128];
};

class com_maple_driver_GP108BridgeUserClient : public IOUserClient {
    OSDeclareDefaultStructors(com_maple_driver_GP108BridgeUserClient)

public:
    virtual bool initWithTask(task_t owningTask, void *securityToken,
                              UInt32 type, OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn clientDied(void) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;

    virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
                                              io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                         IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    virtual IOReturn externalMethod(uint32_t selector,
                                    IOExternalMethodArguments *arguments,
                                    IOExternalMethodDispatch *dispatch,
                                    OSObject *target,
                                    void *reference) APPLE_KEXT_OVERRIDE;

private:
    struct DMASlot {
        IOBufferMemoryDescriptor *desc;
        mach_vm_size_t            size;
        bool                      prepared;
    };

    struct PinnedSlot {
        IOMemoryDescriptor       *desc;
        mach_vm_size_t            size;
        bool                      prepared;
    };

    void releaseAllResources();

    static IOReturn sGetInfo(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sReadVBIOS(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sPCIConfig(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sAllocDMA(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sFreeDMA(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sGetPhysPages(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sPinUserMem(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sUnpinUserMem(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sPowerControl(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sInterruptCtrl(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sReadACPITable(OSObject *target, void *ref, IOExternalMethodArguments *args);

    static const IOExternalMethodDispatch sMethods[kGP108MethodCount];

    task_t                        fTask;
    com_maple_driver_GP108Bridge *fProvider;
    IOLock                       *fClientLock;
    IOSharedDataQueue            *fEventQueue;
    mach_port_t                   fNotificationPort;

    DMASlot                       fDMASlots[kGP108MaxDMASlots];
    PinnedSlot                    fPinnedSlots[kGP108MaxPinnedSlots];
};

#endif /* GP108_BRIDGE_KEXT_HPP */
