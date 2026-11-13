// GERADO por tools/gen_import_stubs.py -- nao edite a mao.
// Stubs de log para os imports do kernel/XAM sem implementacao real em src/kernel/*_exports.cpp.
// Para implementar um import, defina PPC_FUNC(__imp__Nome) num *_exports.cpp e rode o script.

#include "recompiled/ppc_context.h"

#include <atomic>
#include <cstdio>

#define UNIMPLEMENTED_IMPORT(name)                                                         \
    PPC_FUNC(__imp__##name) {                                                              \
        static std::atomic<bool> logged{false};                                            \
        if (!logged.exchange(true)) {                                                      \
            std::fprintf(stderr, "[Kernel] import nao implementado: %s\n", #name);         \
        }                                                                                  \
        ctx.r3.u64 = 0;                                                                    \
    }

UNIMPLEMENTED_IMPORT(XamUserCreateAchievementEnumerator)
UNIMPLEMENTED_IMPORT(XamShowMessageBoxUIEx)
UNIMPLEMENTED_IMPORT(XexGetProcedureAddress)
UNIMPLEMENTED_IMPORT(HalReturnToFirmware)
UNIMPLEMENTED_IMPORT(XamShowMarketplaceDownloadItemsUI)
UNIMPLEMENTED_IMPORT(XamBackgroundDownloadItemGetHistoryStatus)
UNIMPLEMENTED_IMPORT(XamBackgroundDownloadItemGetStatus)
UNIMPLEMENTED_IMPORT(XamLoaderLaunchTitle)
UNIMPLEMENTED_IMPORT(XexLoadImageHeaders)
UNIMPLEMENTED_IMPORT(MmLockAndMapSegmentArray)
UNIMPLEMENTED_IMPORT(MmUnlockAndUnmapSegmentArray)
UNIMPLEMENTED_IMPORT(IoInvalidDeviceRequest)
UNIMPLEMENTED_IMPORT(XeCryptSha)
UNIMPLEMENTED_IMPORT(IoCreateDevice)
UNIMPLEMENTED_IMPORT(IoDeleteDevice)
UNIMPLEMENTED_IMPORT(IoCompleteRequest)
UNIMPLEMENTED_IMPORT(IoCheckShareAccess)
UNIMPLEMENTED_IMPORT(IoSetShareAccess)
UNIMPLEMENTED_IMPORT(IoRemoveShareAccess)
UNIMPLEMENTED_IMPORT(StfsControlDevice)
UNIMPLEMENTED_IMPORT(StfsCreateDevice)
UNIMPLEMENTED_IMPORT(XeKeysConsoleSignatureVerification)
UNIMPLEMENTED_IMPORT(XeKeysConsolePrivateKeySign)
UNIMPLEMENTED_IMPORT(IoDismountVolume)
UNIMPLEMENTED_IMPORT(ExRegisterTitleTerminateNotification)
UNIMPLEMENTED_IMPORT(IoDismountVolumeByFileHandle)
UNIMPLEMENTED_IMPORT(XamCreateEnumeratorHandle)
UNIMPLEMENTED_IMPORT(XamGetPrivateEnumStructureFromHandle)
UNIMPLEMENTED_IMPORT(XamUserGetMembershipTierFromXUID)
UNIMPLEMENTED_IMPORT(XamUserGetOnlineCountryFromXUID)
UNIMPLEMENTED_IMPORT(XNetLogonGetTitleID)
UNIMPLEMENTED_IMPORT(NetDll_WSAStartup)
UNIMPLEMENTED_IMPORT(NetDll_WSACleanup)
UNIMPLEMENTED_IMPORT(NetDll_socket)
UNIMPLEMENTED_IMPORT(NetDll_closesocket)
UNIMPLEMENTED_IMPORT(NetDll_ioctlsocket)
UNIMPLEMENTED_IMPORT(NetDll_bind)
UNIMPLEMENTED_IMPORT(NetDll_listen)
UNIMPLEMENTED_IMPORT(NetDll_accept)
UNIMPLEMENTED_IMPORT(NetDll_recv)
UNIMPLEMENTED_IMPORT(NetDll_send)
UNIMPLEMENTED_IMPORT(NetDll_WSAGetLastError)
UNIMPLEMENTED_IMPORT(NetDll_XNetStartup)
UNIMPLEMENTED_IMPORT(NetDll_XNetCleanup)
UNIMPLEMENTED_IMPORT(XamUserGetDeviceContext)
UNIMPLEMENTED_IMPORT(RmcDeviceRequest)
UNIMPLEMENTED_IMPORT(MicDeviceRequest)
