/** @file
  Copyright (C) 2019, Download-Fritz.  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include <Uefi.h>

#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DiskIo2.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcDevicePathLib.h>
#include <Library/OcGuardLib.h>
#include <Library/UefiBootServicesTableLib.h>

typedef struct {
  UINT32              NumPartitions;
  UINT32              PartitionEntrySize;
  EFI_PARTITION_ENTRY FirstEntry[];
} INTERNAL_PARTITION_ENTRIES;

STATIC EFI_GUID mInternalDiskPartitionEntriesProtocolGuid = {
  0x1A81704, 0x3442, 0x4A7D, { 0x87, 0x40, 0xF4, 0xEC, 0x5B, 0xBE, 0x59, 0x77 }
};

STATIC EFI_GUID mInternalPartitionEntryProtocolGuid = {
  0x9FC6B19, 0xB8A1, 0x4A01, { 0x8D, 0xB1, 0x87, 0x94, 0xE7, 0x63, 0x4C, 0xA5 }
};

STATIC
EFI_STATUS
InternalReadDisk (
  IN  EFI_DISK_IO_PROTOCOL   *DiskIo,
  IN  EFI_DISK_IO2_PROTOCOL  *DiskIo2,
  IN  UINT32                 MediaId,
  IN  UINT64                 Offset,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  ASSERT (DiskIo2 != NULL || DiskIo != NULL);

  if (DiskIo2 != NULL) {
    return DiskIo2->ReadDiskEx (
                      DiskIo2,
                      MediaId,
                      Offset,
                      NULL,
                      BufferSize,
                      Buffer
                      );
  }

  return DiskIo->ReadDisk (DiskIo, MediaId, Offset, BufferSize, Buffer);
}

STATIC
CONST INTERNAL_PARTITION_ENTRIES *
InternalGetDiskPartitions (
  IN EFI_HANDLE  DiskHandle,
  IN BOOLEAN     HasBlockIo2
  )
{
  INTERNAL_PARTITION_ENTRIES *PartEntries;

  EFI_STATUS                 Status;
  BOOLEAN                    Result;

  EFI_BLOCK_IO_PROTOCOL      *BlockIo;
  EFI_BLOCK_IO2_PROTOCOL     *BlockIo2;
  EFI_DISK_IO_PROTOCOL       *DiskIo;
  EFI_DISK_IO2_PROTOCOL      *DiskIo2;
  UINT32                     MediaId;
  UINT32                     BlockSize;

  EFI_LBA                    PartEntryLBA;
  UINT32                     NumPartitions;
  UINT32                     PartEntrySize;
  UINTN                      PartEntriesSize;
  UINTN                      PartEntriesStructSize;
  EFI_PARTITION_TABLE_HEADER *GptHeader;

  ASSERT (DiskHandle != NULL);
  //
  // Retrieve the Block I/O protocol.
  //
  Status = gBS->HandleProtocol (
                  DiskHandle,
                  &mInternalDiskPartitionEntriesProtocolGuid,
                  (VOID **)&PartEntries
                  );
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCPI: Located cached partition entries\n"));
    return PartEntries;
  }

  if (HasBlockIo2) {
    Status = gBS->HandleProtocol (
                    DiskHandle,
                    &gEfiBlockIo2ProtocolGuid,
                    (VOID **)&BlockIo2
                    );
  } else {
    Status = gBS->HandleProtocol (
                    DiskHandle,
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo
                    );
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCPI: Block I/O protocol is not present\n"));
    return NULL;
  }
  //
  // Retrieve the Disk I/O protocol.
  //
  if (HasBlockIo2) {
    DiskIo = NULL;

    Status = gBS->HandleProtocol (
                    DiskHandle,
                    &gEfiDiskIo2ProtocolGuid,
                    (VOID **)&DiskIo2
                    );
  } else {
    DiskIo2 = NULL;

    Status = gBS->HandleProtocol (
                    DiskHandle,
                    &gEfiDiskIoProtocolGuid,
                    (VOID **)&DiskIo
                    );
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCPI: Disk I/O protocol is not present\n"));
    return NULL;
  }
  //
  // Retrieve the GPT header.
  //
  GptHeader = AllocatePool (sizeof (*GptHeader));
  if (GptHeader == NULL) {
    DEBUG ((DEBUG_INFO, "OCPI: GPT header allocation error\n"));
    return NULL;
  }

  if (HasBlockIo2) {
    BlockSize = BlockIo2->Media->BlockSize;
    MediaId   = BlockIo2->Media->MediaId;
  } else {
    BlockSize = BlockIo->Media->BlockSize;
    MediaId   = BlockIo->Media->MediaId;
  }

  Status = InternalReadDisk (
             DiskIo,
             DiskIo2,
             MediaId,
             (PRIMARY_PART_HEADER_LBA * BlockSize),
             sizeof (*GptHeader),
             GptHeader
             );
  if (EFI_ERROR (Status)) {
    FreePool (GptHeader);
    DEBUG ((DEBUG_INFO, "OCPI: ReadDisk1 %r\n", Status));
    return NULL;
  }

  if (GptHeader->Header.Signature != EFI_PTAB_HEADER_ID) {
    FreePool (GptHeader);
    DEBUG ((DEBUG_INFO, "OCPI: Partition table not supported\n"));
    return NULL;
  }

  PartEntrySize = GptHeader->SizeOfPartitionEntry;
  if (PartEntrySize < sizeof (EFI_PARTITION_ENTRY)) {
    FreePool (GptHeader);
    DEBUG ((DEBUG_INFO, "OCPI: GPT header is malformed\n"));
    return NULL;
  }

  NumPartitions = GptHeader->NumberOfPartitionEntries;
  PartEntryLBA  = GptHeader->PartitionEntryLBA;

  FreePool (GptHeader);

  Result = OcOverflowMulUN (NumPartitions, PartEntrySize, &PartEntriesSize);
  if (Result) {
    DEBUG ((DEBUG_INFO, "OCPI: Partition entries size overflows\n"));
    return NULL;
  }

  Result = OcOverflowAddUN (
             sizeof (PartEntries),
             PartEntriesSize,
             &PartEntriesStructSize
             );
  if (Result) {
    DEBUG ((DEBUG_INFO, "OCPI: Partition entries struct size overflows\n"));
    return NULL;
  }
  //
  // Retrieve the GPT partition entries.
  //
  PartEntries = AllocatePool (PartEntriesStructSize);
  if (PartEntries == NULL) {
    DEBUG ((DEBUG_INFO, "OCPI: Partition entries allocation error\n"));
    return NULL;
  }

  Status = InternalReadDisk (
             DiskIo,
             DiskIo2,
             MediaId,
             MultU64x32 (PartEntryLBA, BlockSize),
             PartEntriesSize,
             PartEntries->FirstEntry
             );
  if (EFI_ERROR (Status)) {
    FreePool (PartEntries);
    DEBUG ((DEBUG_INFO, "OCPI: ReadDisk2 %r\n", Status));
    return NULL;
  }

  PartEntries->NumPartitions      = NumPartitions;
  PartEntries->PartitionEntrySize = PartEntrySize;
  //
  // FIXME: This causes the handle to be dangling if the device is detached.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &DiskHandle,
                  &mInternalDiskPartitionEntriesProtocolGuid,
                  PartEntries,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCPI: Failed to cache partition entries\n"));
    FreePool (PartEntries);
    return NULL;
  }

  return PartEntries;
}

/**
  Retrieve the partition's GPT information, if applicable

  @param[in] FsHandle  The device handle of the partition to retrieve info of.

**/
CONST EFI_PARTITION_ENTRY *
OcGetGptPartitionEntry (
  IN EFI_HANDLE  FsHandle
  )
{
  CONST EFI_PARTITION_ENTRY        *PartEntry;
  CONST INTERNAL_PARTITION_ENTRIES *Partitions;

  EFI_STATUS                       Status;
  EFI_DEVICE_PATH_PROTOCOL         *FsDevicePath;
  CONST HARDDRIVE_DEVICE_PATH      *HdNode;
  EFI_DEVICE_PATH_PROTOCOL         *PrefixPath;
  EFI_DEVICE_PATH_PROTOCOL         *TempPath;
  UINTN                            HdNodeOffset;
  EFI_HANDLE                       DiskHandle;
  BOOLEAN                          HasBlockIo2;
  UINTN                            Offset;

  ASSERT (FsHandle != NULL);

  Status = gBS->HandleProtocol (
                  FsHandle,
                  &mInternalPartitionEntryProtocolGuid,
                  (VOID **)&PartEntry
                  );
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCPI: Located cached partition entry\n"));
    return PartEntry;
  }
  //
  // Retrieve the partition Device Path information.
  //
  FsDevicePath = DevicePathFromHandle (FsHandle);
  if (FsDevicePath == NULL) {
    DEBUG ((DEBUG_INFO, "OCPI: Failed to retrieve Device Path\n"));
    return NULL;
  }

  HdNode = (HARDDRIVE_DEVICE_PATH *)(
             FindDevicePathNodeWithType (
               FsDevicePath,
               MEDIA_DEVICE_PATH,
               MEDIA_HARDDRIVE_DP
               )
             );
  if (HdNode == NULL) {
    return NULL;
  }

  PrefixPath = DuplicateDevicePath (FsDevicePath);
  if (PrefixPath == NULL) {
    DEBUG ((DEBUG_INFO, "OCPI: DP allocation error\n"));
    return NULL;
  }
  //
  // Strip the HD node in order to retrieve the last node supporting Block I/O
  // before it, which is going to be its disk.
  //
  HdNodeOffset = ((UINTN)HdNode - (UINTN)FsDevicePath);
  TempPath = (EFI_DEVICE_PATH_PROTOCOL *)((UINTN)PrefixPath + HdNodeOffset);
  SetDevicePathEndNode (TempPath);
  
  TempPath = PrefixPath;
  Status = gBS->LocateDevicePath (
                  &gEfiBlockIo2ProtocolGuid,
                  &TempPath,
                  &DiskHandle
                  );
  HasBlockIo2 = !EFI_ERROR (Status);
  if (!HasBlockIo2) {
    TempPath = PrefixPath;
    Status = gBS->LocateDevicePath (
                    &gEfiBlockIoProtocolGuid,
                    &TempPath,
                    &DiskHandle
                    );
  }
  if (EFI_ERROR (Status)) {
    DebugPrintDevicePath (
      DEBUG_INFO,
      "OCPI: Failed to locate disk",
      PrefixPath
      );
  }

  FreePool (PrefixPath);

  if (EFI_ERROR (Status)) {
    return NULL;
  }
  //
  // Get the disk's GPT partition entries.
  //
  Partitions = InternalGetDiskPartitions (DiskHandle, HasBlockIo2);
  if (Partitions == NULL) {
    DEBUG ((DEBUG_INFO, "OCPI: Failed to retrieve disk info\n"));
    return NULL;
  }

  if (HdNode->PartitionNumber > Partitions->NumPartitions) {
    DEBUG ((DEBUG_INFO, "OCPI: Partition is OOB\n"));
    return NULL;
  }

  Offset = ((UINTN)HdNode->PartitionNumber * Partitions->PartitionEntrySize);
  PartEntry = (EFI_PARTITION_ENTRY *)((UINTN)Partitions->FirstEntry + Offset);
  //
  // FIXME: This causes the handle to be dangling if the device is detached.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &FsHandle,
                  &mInternalPartitionEntryProtocolGuid,
                  PartEntry,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCPI: Failed to cache partition entry\n"));
    return NULL;
  }

  return PartEntry;
}