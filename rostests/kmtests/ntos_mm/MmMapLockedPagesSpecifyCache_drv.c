/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Test driver for MmMapLockedPagesSpecifyCache function
 * PROGRAMMER:      Pierre Schweitzer <pierre@reactos.org>
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#include "MmMapLockedPagesSpecifyCache.h"

static KMT_IRP_HANDLER TestIrpHandler;
static KMT_MESSAGE_HANDLER TestMessageHandler;

static PVOID CurrentBuffer;
static PMDL CurrentMdl;
static PVOID CurrentUser;

NTSTATUS
TestEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PCUNICODE_STRING RegistryPath,
    OUT PCWSTR *DeviceName,
    IN OUT INT *Flags)
{
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(Flags);

    *DeviceName = L"MmMapLockedPagesSpecifyCache";

    KmtRegisterIrpHandler(IRP_MJ_CLEANUP, NULL, TestIrpHandler);
    KmtRegisterMessageHandler(0, NULL, TestMessageHandler);

    return Status;
}

VOID
TestUnload(
    IN PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
}

VOID
TestCleanEverything(VOID)
{
    NTSTATUS SehStatus;

    if (CurrentMdl == NULL)
    {
        return;
    }

    if (CurrentUser != NULL)
    {
        SehStatus = STATUS_SUCCESS;
        _SEH2_TRY
        {
            MmUnmapLockedPages(CurrentUser, CurrentMdl);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            SehStatus = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        ok_eq_hex(SehStatus, STATUS_SUCCESS);
        CurrentUser = NULL;
    }

    SehStatus = STATUS_SUCCESS;
    _SEH2_TRY
    {
        MmUnlockPages(CurrentMdl);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SehStatus = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok_eq_hex(SehStatus, STATUS_SUCCESS);
    IoFreeMdl(CurrentMdl);
    ExFreePoolWithTag(CurrentBuffer, 'MLPC');
    CurrentMdl = NULL;
}

static
NTSTATUS
TestMessageHandler(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG ControlCode,
    IN PVOID Buffer OPTIONAL,
    IN SIZE_T InLength,
    IN OUT PSIZE_T OutLength)
{
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS SehStatus;

    switch (ControlCode)
    {
        case IOCTL_QUERY_BUFFER:
        {
            ok(Buffer != NULL, "Buffer is NULL\n");
            ok_eq_size(InLength, sizeof(QUERY_BUFFER));
            ok_eq_size(*OutLength, sizeof(QUERY_BUFFER));
            ok_eq_pointer(CurrentMdl, NULL);
            ok(ExGetPreviousMode() == UserMode, "Not comming from umode!\n");
            if (!skip(Buffer && InLength >= sizeof(QUERY_BUFFER) && *OutLength >= sizeof(QUERY_BUFFER), "Cannot read/write from/to buffer!\n"))
            {
                PQUERY_BUFFER QueryBuffer;
                USHORT Length;
                MEMORY_CACHING_TYPE CacheType;

                QueryBuffer = Buffer;
                CacheType = (QueryBuffer->Cached ? MmCached : MmNonCached);
                Length = QueryBuffer->Length;
                ok(Length > 0, "Null size!\n");

                CurrentBuffer = ExAllocatePoolWithTag(NonPagedPool, Length + 0x8, 'MLPC');
                ok(CurrentBuffer != NULL, "ExAllocatePool failed!\n");
                CurrentUser = NULL;
                if (!skip(CurrentBuffer != NULL, "ExAllocatePool failed!\n"))
                {
                    CurrentMdl = IoAllocateMdl(CurrentBuffer, Length, FALSE, FALSE, NULL);
                    ok(CurrentMdl != NULL, "IoAllocateMdl failed!\n");
                    if (CurrentMdl)
                    {
                        KIRQL Irql;

                        SehStatus = STATUS_SUCCESS;
                        _SEH2_TRY
                        {
                            MmProbeAndLockPages(CurrentMdl, KernelMode, IoWriteAccess);
                        }
                        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
                        {
                            SehStatus = _SEH2_GetExceptionCode();
                        }
                        _SEH2_END;
                        ok_eq_hex(SehStatus, STATUS_SUCCESS);

                        Irql = KeGetCurrentIrql();
                        ok(Irql <= APC_LEVEL, "IRQL > APC_LEVEL: %d\n", Irql);

                        SehStatus = STATUS_SUCCESS;
                        _SEH2_TRY
                        {
                            CurrentUser = MmMapLockedPagesSpecifyCache(CurrentMdl, UserMode, CacheType, NULL, FALSE, NormalPagePriority);
                        }
                        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
                        {
                            SehStatus = _SEH2_GetExceptionCode();
                        }
                        _SEH2_END;

                        if (QueryBuffer->Cached)
                        {
                            ok_eq_hex(SehStatus, STATUS_SUCCESS);
                            ok(CurrentUser != NULL, "MmMapLockedPagesSpecifyCache failed!\n");
                        }
                        else
                        {
                            ok_eq_hex(SehStatus, STATUS_INVALID_ADDRESS);
                            ok_eq_pointer(CurrentUser, NULL);
                        }
                    }
                }
                QueryBuffer->Buffer = CurrentUser;

                *OutLength = sizeof(QUERY_BUFFER);
            }

            break;
        }
        case IOCTL_READ_BUFFER:
        {
            ok(Buffer != NULL, "Buffer is NULL\n");
            ok_eq_size(InLength, sizeof(READ_BUFFER));
            ok_eq_size(*OutLength, 0);
            ok(CurrentMdl != NULL, "MDL is not in use!\n");

            if (!skip(Buffer && InLength >= sizeof(QUERY_BUFFER), "Cannot read from buffer!\n"))
            {
                PREAD_BUFFER ReadBuffer;

                ReadBuffer = Buffer;
                if (!skip(ReadBuffer && ReadBuffer->Buffer == CurrentUser, "Cannot find matching MDL\n"))
                {
                    if (ReadBuffer->Buffer != NULL)
                    {
                        USHORT i;
                        PUSHORT KBuffer = MmGetSystemAddressForMdlSafe(CurrentMdl, NormalPagePriority);
                        ok(KBuffer != NULL, "Failed to get kmode ptr\n");

                        if (!skip(Buffer != NULL, "Failed to get kmode ptr\n"))
                        {
                            for (i = 0; i < ReadBuffer->Length / sizeof(USHORT); ++i)
                            {
                                ok_eq_ulong((ULONG)KBuffer[i], (ULONG)ReadBuffer->Pattern);
                            }
                        }
                    }
                }

                TestCleanEverything();
            }

            break;
        }
        default:
            ok(0, "Got an unknown message! DeviceObject=%p, ControlCode=%lu, Buffer=%p, In=%lu, Out=%lu bytes\n",
                    DeviceObject, ControlCode, Buffer, InLength, *OutLength);
            break;
    }

    return Status;
}

static
NTSTATUS
TestIrpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack)
{
    NTSTATUS Status;

    PAGED_CODE();

    DPRINT("IRP %x/%x\n", IoStack->MajorFunction, IoStack->MinorFunction);
    ASSERT(IoStack->MajorFunction == IRP_MJ_CLEANUP);

    Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;

    if (IoStack->MajorFunction == IRP_MJ_CLEANUP)
    {
        TestCleanEverything();
        Status = STATUS_SUCCESS;
    }

    if (Status == STATUS_PENDING)
    {
        IoMarkIrpPending(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        Status = STATUS_PENDING;
    }
    else
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    return Status;
}
