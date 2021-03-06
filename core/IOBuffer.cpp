#include <errno.h>
#include <endian.h>
#include "IOBuffer.h"

using namespace fpnn;

bool RecvBuffer::entryEncryptMode(uint8_t *key, size_t key_len, uint8_t *iv, bool streamMode)
{
	if (_receivedPackage > 1)
		return false;

	delete _receiver;
	if (streamMode)
		_receiver = new EncryptedStreamReceiver(key, key_len, iv);
	else
		_receiver = new EncryptedPackageReceiver(key, key_len, iv);

	return true;
}

void RecvBuffer::entryWebSocketMode()
{
	delete _receiver;
	_receiver = new WebSocketReceiver();
}

void SendBuffer::encryptData()
{
	if (_sentPackage > 0)
		_encryptor->encrypt(_currBuffer);
	else
	{
		if (!_encryptAfterFirstPackage)
			_encryptor->encrypt(_currBuffer);
	}
}

int SendBuffer::realSend(int fd, bool& needWaitSendEvent)
{
	uint64_t currSendBytes = 0;

	needWaitSendEvent = false;
	while (true)
	{
		if (_currBuffer == NULL)
		{
			CurrBufferProcessFunc currBufferProcess;
			{
				std::unique_lock<std::mutex> lck(*_mutex);
				if (_outQueue.size() == 0)
				{
					_sentBytes += currSendBytes;
					_sendToken = true;
					return 0;
				}

				_currBuffer = _outQueue.front();
				_outQueue.pop();
				_offset = 0;

				currBufferProcess = _currBufferProcess;
			}

			if (currBufferProcess)
				(this->*currBufferProcess)();
		}

		size_t requireSend = _currBuffer->length() - _offset;
		ssize_t sendBytes = write(fd, _currBuffer->data() + _offset, requireSend);
		if (sendBytes == -1)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				needWaitSendEvent = true;
				std::unique_lock<std::mutex> lck(*_mutex);
				_sentBytes += currSendBytes;
				_sendToken = true;
				return 0;
			}
			if (errno == EINTR)
				continue;

			std::unique_lock<std::mutex> lck(*_mutex);
			_sentBytes += currSendBytes;
			_sendToken = true;
			return errno;
		}
		else
		{
			_offset += (size_t)sendBytes;
			currSendBytes += (uint64_t)sendBytes;
			if (_offset == _currBuffer->length())
			{
				delete _currBuffer;
				_currBuffer = NULL;
				_offset = 0;
				_sentPackage += 1;
			}
		}
	}
}

int SendBuffer::send(int fd, bool& needWaitSendEvent, std::string* data)
{
	if (data && data->empty())
	{
		delete data;
		data = NULL;
	}

	{
		std::unique_lock<std::mutex> lck(*_mutex);
		if (data)
			_outQueue.push(data);

		if (!_sendToken)
			return 0;

		_sendToken = false;
	}

	//-- Token will be return in realSend() function.
	int err = realSend(fd, needWaitSendEvent); 	//-- ignore all error status. it will be deal in IO thread.
	return err;
}

bool SendBuffer::entryEncryptMode(uint8_t *key, size_t key_len, uint8_t *iv, bool streamMode)
{
	if (_encryptor)
		return false;

	Encryptor* encryptor = NULL;
	if (streamMode)
		encryptor = new StreamEncryptor(key, key_len, iv);
	else
		encryptor = new PackageEncryptor(key, key_len, iv);

	{
		std::unique_lock<std::mutex> lck(*_mutex);
		if (_sentBytes) return false;
		if (_sendToken == false) return false;

		_encryptor = encryptor;
		_currBufferProcess = &SendBuffer::encryptData;
	}

	return true;
}

void SendBuffer::appendData(std::string* data)
{
	if (data && data->empty())
	{
		delete data;
		return;
	}

	std::unique_lock<std::mutex> lck(*_mutex);
	if (data)
		_outQueue.push(data);
}

void SendBuffer::entryWebSocketMode(std::string* data)
{
	std::unique_lock<std::mutex> lck(*_mutex);
	_currBufferProcess = &SendBuffer::addWebSocketWrap;

	if (data)
	{
		if (data->size())
			_outQueue.push(data);
		else
			delete data;
	}
}

void SendBuffer::addWebSocketWrap()
{
	if (_sentPackage == 0)
		return;
	/*
		RFC6455 Required:
		A server MUST NOT mask any frames that it sends to the client. -- rfc6455: 5.1.
	*/

	size_t dataSize = _currBuffer->length();
	if (dataSize > 1)
	{
		size_t maxLen = dataSize + 2 + 8;	//-- data-szie + header + extended payload length. Without masking-key field.

		std::string wrappedData;
		wrappedData.reserve(maxLen);

		uint8_t headerBuffer[2] = { 0x82, 0x0 };		//-- Binary frame. Only one frame. beacuse all FPNN data MUST less than max value of size_t. 

		if (dataSize <= 126)
		{
			headerBuffer[1] = (uint8_t)dataSize;
			wrappedData.append((char*)headerBuffer, 2);
		}
		else if (dataSize <= 0xffff)
		{
			uint16_t payloadSize = (uint16_t)dataSize;
			payloadSize = htobe16(payloadSize);

			headerBuffer[1] = 126;
			wrappedData.append((char*)headerBuffer, 2);
			wrappedData.append((char*)&payloadSize, 2);
		}
		else
		{
			uint64_t payloadSize = dataSize;
			payloadSize = htobe64(payloadSize);

			headerBuffer[1] = 126;
			wrappedData.append((char*)headerBuffer, 2);
			wrappedData.append((char*)&payloadSize, 8);
		}

		wrappedData.append(*_currBuffer);
		_currBuffer->swap(wrappedData);
	}
	else	//-- control frames
	{
		uint8_t buf[2] = {0x0, 0x0};
		buf[0] = 0x80 | _currBuffer->at(0);

		_currBuffer->assign((char*)buf, 2);
	}
}
