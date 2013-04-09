#include "CmD3D11HardwareBuffer.h"
#include "CmD3D11Mappings.h"
#include "CmD3D11Device.h"
#include "CmException.h"
#include "CmDebug.h"

namespace CamelotEngine
{
	D3D11HardwareBuffer::D3D11HardwareBuffer(BufferType btype, GpuBufferUsage usage, UINT32 elementCount, UINT32 elementSize, 
		D3D11Device& device, bool useSystemMem, bool streamOut, bool randomGpuWrite, bool useCounter)
		: HardwareBuffer(usage, useSystemMem),
		mD3DBuffer(0),
		mpTempStagingBuffer(0),
		mUseTempStagingBuffer(false),
		mBufferType(btype),
		mDevice(device),
		mElementCount(elementCount),
		mElementSize(elementSize),
		mRandomGpuWrite(randomGpuWrite),
		mUseCounter(useCounter)
	{
		assert((!streamOut || btype == BT_VERTEX) && "Stream out flag is only supported on vertex buffers");
		assert(!randomGpuWrite || (btype & BT_GROUP_GENERIC) != 0 && "randomGpuWrite flag can only be enabled with append/consume, indirect argument, structured or raw buffers");
		assert(btype != BT_APPENDCONSUME || randomGpuWrite && "Append/Consume buffer must be created with randomGpuWrite enabled.");
		assert(!useCounter || btype == BT_STRUCTURED && "Counter can only be used with a structured buffer.");
		assert(!useCounter || randomGpuWrite && "Counter can only be used with buffers that have randomGpuWrite enabled.");
		assert(!randomGpuWrite || !useSystemMem && "randomGpuWrite and useSystemMem cannot be used together.");
		assert(!(useSystemMem && streamOut) && "useSystemMem and streamOut cannot be used together.");

		mSizeInBytes = elementCount * elementSize;
		mDesc.ByteWidth = mSizeInBytes;
		mDesc.MiscFlags = 0;
		mDesc.StructureByteStride = 0;

		if (useSystemMem)
		{
			mDesc.Usage = D3D11_USAGE_STAGING;
			mDesc.BindFlags = 0;
			mDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ ;
		}
		else if(randomGpuWrite)
		{
			mDesc.Usage = D3D11_USAGE_DEFAULT;
			mDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			mDesc.CPUAccessFlags = 0;
		}
		else
		{
			mDesc.Usage = D3D11Mappings::_getUsage(mUsage);
			mDesc.CPUAccessFlags = D3D11Mappings::_getAccessFlags(mUsage); 

			switch(btype)
			{
			case BT_VERTEX:
				mDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				if (streamOut)
					mDesc.BindFlags |= D3D11_BIND_STREAM_OUTPUT;
				break;
			case BT_INDEX:
				mDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
				break;
			case BT_CONSTANT:
				mDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				break;
			case BT_STRUCTURED:
			case BT_APPENDCONSUME:
				mDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				mDesc.StructureByteStride = elementSize;
				mDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				break;
			case BT_RAW:
				mDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				mDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
				break;
			case BT_INDIRECTARGUMENT:
				mDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				mDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
				break;
			}
		}

		HRESULT hr = device.getD3D11Device()->CreateBuffer( &mDesc, nullptr, &mD3DBuffer );
		if (FAILED(hr) || mDevice.hasError())
		{
			String msg = device.getErrorDescription();
			CM_EXCEPT(RenderingAPIException, "Cannot create D3D11 buffer: " + msg);
		}
	}

	D3D11HardwareBuffer::~D3D11HardwareBuffer()
	{
		SAFE_RELEASE(mD3DBuffer);

		if(mpTempStagingBuffer != nullptr)
			CM_DELETE(mpTempStagingBuffer, D3D11HardwareBuffer, PoolAlloc);
	}

	void* D3D11HardwareBuffer::lockImpl(UINT32 offset, 
		UINT32 length, GpuLockOptions options)
	{
		if (length > mSizeInBytes)
			CM_EXCEPT(RenderingAPIException, "Provided length " + toString(length) + " larger than the buffer " + toString(mSizeInBytes) + ".");		

		// Use direct (and faster) Map/Unmap if dynamic write, or a staging read/write
		if((mDesc.Usage == D3D11_USAGE_DYNAMIC && options != GBL_READ_ONLY) || mDesc.Usage == D3D11_USAGE_STAGING)
		{
			D3D11_MAP mapType;

			switch(options)
			{
			case GBL_WRITE_ONLY_DISCARD:
				if (mUsage & GBU_DYNAMIC)
				{
					// Map cannot be called with MAP_WRITE access, 
					// because the Resource was created as D3D11_USAGE_DYNAMIC. 
					// D3D11_USAGE_DYNAMIC Resources must use either MAP_WRITE_DISCARD 
					// or MAP_WRITE_NO_OVERWRITE with Map.
					mapType = D3D11_MAP_WRITE_DISCARD;
				}
				else
				{
					// Map cannot be called with MAP_WRITE_DISCARD access, 
					// because the Resource was not created as D3D11_USAGE_DYNAMIC. 
					// D3D11_USAGE_DYNAMIC Resources must use either MAP_WRITE_DISCARD 
					// or MAP_WRITE_NO_OVERWRITE with Map.
					mapType = D3D11_MAP_WRITE;

					LOGWRN("DISCARD lock is only available on dynamic buffers. Falling back to normal write.");
				}
				break;
			case GBL_WRITE_ONLY_NO_OVERWRITE:
				if(mBufferType == BT_INDEX || mBufferType == BT_VERTEX)
					mapType = D3D11_MAP_WRITE_NO_OVERWRITE;
				else
				{
					mapType = D3D11_MAP_WRITE;

					LOGWRN("NO_OVERWRITE lock is not available on this (" + toString(mBufferType) + ") buffer type. Falling back to normal write.");
				}
				break;
			case GBL_WRITE_ONLY:
				mapType = D3D11_MAP_WRITE;
				break;
			case GBL_READ_WRITE:
				if ((mDesc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) != 0 &&
					(mDesc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) != 0)
				{
					mapType = D3D11_MAP_READ_WRITE;
				}
				else if(mDesc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)
				{
					mapType = D3D11_MAP_WRITE;
				}
				else
				{
					mapType = D3D11_MAP_READ;
				}
				break;
			case GBL_READ_ONLY:
				mapType = D3D11_MAP_READ;
				break;
			}

			if(D3D11Mappings::isMappingRead(mapType) && (mDesc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) == 0)
				CM_EXCEPT(RenderingAPIException, "Trying to read a buffer, but buffer wasn't created with a read access flag.");

			if(D3D11Mappings::isMappingWrite(mapType) && (mDesc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) == 0)
				CM_EXCEPT(RenderingAPIException, "Trying to write to a buffer, but buffer wasn't created with a write access flag.");

			void * pRet = NULL;
			D3D11_MAPPED_SUBRESOURCE mappedSubResource;
			mappedSubResource.pData = NULL;
			mDevice.clearErrors();
			HRESULT hr = mDevice.getImmediateContext()->Map(mD3DBuffer, 0, mapType, 0, &mappedSubResource);
			if (FAILED(hr) || mDevice.hasError())
			{
				String msg = mDevice.getErrorDescription();
				CM_EXCEPT(RenderingAPIException, "Error calling Map: " + msg);
			}

			pRet = static_cast<void*>(static_cast<char*>(mappedSubResource.pData) + offset);

			return pRet;
		}
		else // Otherwise create a staging buffer to do all read/write operations on. Usually try to avoid this.
		{
			mUseTempStagingBuffer = true;
			if (!mpTempStagingBuffer)
			{
				// create another buffer instance but use system memory
				mpTempStagingBuffer = CM_NEW(D3D11HardwareBuffer, PoolAlloc) D3D11HardwareBuffer(mBufferType, mUsage, 1, mSizeInBytes, mDevice, true);
			}

			// schedule a copy to the staging
			if (options != GBL_WRITE_ONLY_DISCARD)
				mpTempStagingBuffer->copyData(*this, 0, 0, mSizeInBytes, true);

			// register whether we'll need to upload on unlock
			mStagingUploadNeeded = (options != GBL_READ_ONLY);

			return mpTempStagingBuffer->lock(offset, length, options);
		}
	}

	void D3D11HardwareBuffer::unlockImpl(void)
	{
		if (mUseTempStagingBuffer)
		{
			mUseTempStagingBuffer = false;

			// ok, we locked the staging buffer
			mpTempStagingBuffer->unlock();

			// copy data if needed
			// this is async but driver should keep reference
			if (mStagingUploadNeeded)
				copyData(*mpTempStagingBuffer, 0, 0, mSizeInBytes, true);

			// delete
			// not that efficient, but we should not be locking often
			if(mpTempStagingBuffer != nullptr)
			{
				CM_DELETE(mpTempStagingBuffer, D3D11HardwareBuffer, PoolAlloc);
				mpTempStagingBuffer = nullptr;
			}
		}
		else
		{
			// unmap
			mDevice.getImmediateContext()->Unmap(mD3DBuffer, 0);
		}
	}

	void D3D11HardwareBuffer::copyData(HardwareBuffer& srcBuffer, UINT32 srcOffset, 
		UINT32 dstOffset, UINT32 length, bool discardWholeBuffer)
	{
		// If we're copying same-size buffers in their entirety...
		if (srcOffset == 0 && dstOffset == 0 &&
			length == mSizeInBytes && mSizeInBytes == srcBuffer.getSizeInBytes())
		{
			// schedule hardware buffer copy
			mDevice.getImmediateContext()->CopyResource(mD3DBuffer, static_cast<D3D11HardwareBuffer&>(srcBuffer).getD3DBuffer());
			if (mDevice.hasError())
			{
				String errorDescription = mDevice.getErrorDescription();
				CM_EXCEPT(RenderingAPIException, "Cannot copy D3D11 resource\nError Description:" + errorDescription);
			}
		}
		else
		{
			// copy subregion
			D3D11_BOX srcBox;
			srcBox.left = (UINT)srcOffset;
			srcBox.right = (UINT)srcOffset + length;
			srcBox.top = 0;
			srcBox.bottom = 1;
			srcBox.front = 0;
			srcBox.back = 1;

			mDevice.getImmediateContext()->CopySubresourceRegion(mD3DBuffer, 0, (UINT)dstOffset, 0, 0, 
				static_cast<D3D11HardwareBuffer&>(srcBuffer).getD3DBuffer(), 0, &srcBox);
			if (mDevice.hasError())
			{
				String errorDescription = mDevice.getErrorDescription();
				CM_EXCEPT(RenderingAPIException, "Cannot copy D3D11 subresource region\nError Description:" + errorDescription);
			}
		}
	}

	void D3D11HardwareBuffer::readData(UINT32 offset, UINT32 length, void* pDest)
	{
		// There is no functional interface in D3D, just do via manual 
		// lock, copy & unlock
		void* pSrc = this->lock(offset, length, GBL_READ_ONLY);
		memcpy(pDest, pSrc, length);
		this->unlock();
	}

	void D3D11HardwareBuffer::writeData(UINT32 offset, UINT32 length, 
		const void* pSource, bool discardWholeBuffer)
	{
		if(mDesc.Usage == D3D11_USAGE_DYNAMIC || mDesc.Usage == D3D11_USAGE_STAGING)
		{
			void* pDst = this->lock(offset, length, 
				discardWholeBuffer ? GBL_WRITE_ONLY_DISCARD : GBL_WRITE_ONLY);
			memcpy(pDst, pSource, length);
			this->unlock();
		}
		else if(mDesc.Usage == D3D11_USAGE_DEFAULT)
		{
			mDevice.getImmediateContext()->UpdateSubresource(mD3DBuffer, 0, nullptr, pSource, offset, length);
		}
		else
		{
			CM_EXCEPT(RenderingAPIException, "Trying to write into a buffer with unsupported usage: " + toString(mDesc.Usage));
		}
	}
}