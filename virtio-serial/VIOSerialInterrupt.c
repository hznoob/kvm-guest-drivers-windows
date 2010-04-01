/**********************************************************************
 * Copyright (c) 2010  Red Hat, Inc.
 *
 * File: VIOSerialInterrupt.c
 *
 * Placeholder for the interrupt handling related functions
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
**********************************************************************/

#include "osdep.h"
#include "kdebugprint.h"
#include "VIOSerialDriver.h"
#include "VIOSerialDevice.h"
#include "VIOSerialCore.h"
#include "VIOSerialCoreQueue.h"

BOOLEAN VIOSerialInterruptIsr(IN WDFINTERRUPT Interrupt,
							  IN ULONG MessageID)
{
	PDEVICE_CONTEXT	pContext = GetDeviceContext(WdfInterruptGetDevice(Interrupt));
	ULONG status = 0;
	BOOLEAN b;

	if(!pContext->isDeviceInitialized)
	{
		return FALSE;
	}

	status = VirtIODeviceISR(&pContext->IODevice);
	if(status == VIRTIO_SERIAL_INVALID_INTERRUPT_STATUS)
	{
		status = 0;
	}

	if(!!status)
	{
		DPrintf(6, ("Got ISR - it is ours %d!\n", status));
		WdfInterruptQueueDpcForIsr(Interrupt);
	}

	return !!status;
}

VOID VIOSerialInterruptDpc(IN WDFINTERRUPT Interrupt,
						   IN WDFOBJECT AssociatedObject)
{
	//TBD handle the transfer
	unsigned int len;
	unsigned int i;
	pIODescriptor pBufferDescriptor;
	PDEVICE_CONTEXT	pContext;
	
	
	DEBUG_ENTRY(0);

	if(!Interrupt)
	{
		DPrintf(0, ("Got NULL interrupt object DPC!\n"));
		return;
	}

	pContext = GetDeviceContext(WdfInterruptGetDevice(Interrupt));
	WdfSpinLockAcquire(pContext->DPCLock);
	//Get consumed buffers for transmit queues
	for(i = 0; i < pContext->consoleConfig.nr_ports; i++ )
	{
		if(pContext->SerialPorts[i].SendQueue)
		{
			while(pBufferDescriptor = pContext->SerialPorts[i].SendQueue->vq_ops->get_buf(pContext->SerialPorts[i].SendQueue, &len))
			{
				RemoveEntryList(&pBufferDescriptor->listEntry); // Remove from in use list
				InsertTailList(&pContext->SerialPorts[i].SendFreeBuffers, &pBufferDescriptor->listEntry);
			}
		}

		if(pContext->SerialPorts[i].lastReadRequest &&
		   pContext->SerialPorts[i].ReceiveQueue)
		{
			size_t size;
			WDFMEMORY outMemory;
			NTSTATUS status;
			NTSTATUS cancelationStatus;

			if(NT_SUCCESS(status = WdfRequestRetrieveOutputMemory(pContext->SerialPorts[i].lastReadRequest, &outMemory)))
			{
				if(STATUS_UNSUCCESSFUL != (status = VSCRecieveCopyBuffer(&pContext->SerialPorts[i],
																		 &outMemory,
																		 &size,
																		 pContext->DPCLock,
																		 TRUE)))
				{
					cancelationStatus = WdfRequestUnmarkCancelable(pContext->SerialPorts[i].lastReadRequest);
					if(cancelationStatus != STATUS_CANCELLED) 
					{
						DPrintf(0, ("Complete pending read %x\n", pContext->SerialPorts[i].lastReadRequest));
						WdfSpinLockRelease(pContext->DPCLock);
						WdfRequestCompleteWithInformation(pContext->SerialPorts[i].lastReadRequest,
														  status,
														  size);
						WdfSpinLockAcquire(pContext->DPCLock);
					}

					pContext->SerialPorts[i].lastReadRequest = NULL;
				}
			}
			else
			{
				WdfSpinLockRelease(pContext->DPCLock);
				WdfRequestCompleteWithInformation(pContext->SerialPorts[i].lastReadRequest,
												  status,
												  size);
				WdfSpinLockAcquire(pContext->DPCLock);
				pContext->SerialPorts[i].lastReadRequest = NULL;
			}
		}
	}

	//Get control messages
	if(pContext->SerialPorts[VIRTIO_SERIAL_CONTROL_PORT_INDEX].ReceiveQueue)
	{
		if(pBufferDescriptor = pContext->SerialPorts[VIRTIO_SERIAL_CONTROL_PORT_INDEX].ReceiveQueue->vq_ops->get_buf(pContext->SerialPorts[VIRTIO_SERIAL_CONTROL_PORT_INDEX].ReceiveQueue, &len))
		{
			DPrintf(0, ("Got control message\n"));
			//HandleIncomingControlMessage(pBufferDescriptor->DataInfo.Virtual, len);

			//Return the buffer to usage... - if we handle the mesages in workitem, the below line should move there
			pBufferDescriptor->DataInfo.size = PAGE_SIZE;
			AddRxBufferToQueue(&pContext->SerialPorts[VIRTIO_SERIAL_CONTROL_PORT_INDEX], pBufferDescriptor);
			pContext->SerialPorts[VIRTIO_SERIAL_CONTROL_PORT_INDEX].ReceiveQueue->vq_ops->kick(pContext->SerialPorts[VIRTIO_SERIAL_CONTROL_PORT_INDEX].ReceiveQueue);
		}
	}

	WdfSpinLockRelease(pContext->DPCLock);
}

static VOID VIOSerialEnableDisableInterrupt(PDEVICE_CONTEXT pContext,
											IN BOOLEAN bEnable)
{
	unsigned int i;

	DEBUG_ENTRY(0);

	if(!pContext)
		return;

	for(i = 0; i < pContext->consoleConfig.nr_ports; i++ )
	{
		if(pContext->SerialPorts[i].ReceiveQueue)
		{
			pContext->SerialPorts[i].ReceiveQueue->vq_ops->enable_interrupt(pContext->SerialPorts[i].ReceiveQueue, bEnable);
		}

		if(pContext->SerialPorts[i].SendQueue)
		{
			pContext->SerialPorts[i].SendQueue->vq_ops->enable_interrupt(pContext->SerialPorts[i].SendQueue, bEnable);
		}
	}

	if(bEnable) // Also kick
	{
		for(i = 0; i < pContext->consoleConfig.nr_ports; i++ )
		{
			if(pContext->SerialPorts[i].ReceiveQueue)
			{
				pContext->SerialPorts[i].ReceiveQueue->vq_ops->kick(pContext->SerialPorts[i].ReceiveQueue);
			}

			if(pContext->SerialPorts[i].SendQueue)
			{
				pContext->SerialPorts[i].SendQueue->vq_ops->kick(pContext->SerialPorts[i].SendQueue);
			}
		}
	}
}

NTSTATUS VIOSerialInterruptEnable(IN WDFINTERRUPT Interrupt,
								  IN WDFDEVICE AssociatedDevice)
{
	DEBUG_ENTRY(0);
	VIOSerialEnableDisableInterrupt(GetDeviceContext(WdfInterruptGetDevice(Interrupt)), 
									TRUE);

	return STATUS_SUCCESS;
}

NTSTATUS VIOSerialInterruptDisable(IN WDFINTERRUPT Interrupt,
								   IN WDFDEVICE AssociatedDevice)
{
	DEBUG_ENTRY(0);
	VIOSerialEnableDisableInterrupt(GetDeviceContext(WdfInterruptGetDevice(Interrupt)),
									FALSE);

	return STATUS_SUCCESS;
}
