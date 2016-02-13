#include "FrameBuffer.h"

#include "Constants.h"

namespace ofxSquashBuddies {
#pragma mark FrameBuffer
	//---------
	FrameBuffer::FrameBuffer(ofThreadChannel<Message> & decompressorToFrameReceiver) :
	decompressorToFrameReceiver(decompressorToFrameReceiver)
	{		
		this->clear();

		this->decompressThread = thread([this]() {
			this->decompressLoop();
		});
	}

	//---------
	FrameBuffer::~FrameBuffer() {
		this->threadsRunning = false;
		this->bufferToDecompressor.close();

		if (this->decompressThread.joinable()) {
			this->decompressThread.join();
		}
	}

	//---------
	void FrameBuffer::setCodec(const ofxSquash::Codec & codec) {
		this->codec = codec;

		if (this->packetIndexPosition == 0) {
			//reset if we haven't started processing packets yet
			this->clear();
		}
	}

	//---------
	void FrameBuffer::setFrameIndex(uint32_t frameIndex) {
		this->frameIndex = frameIndex;
	}

	//---------
	uint32_t FrameBuffer::getFrameIndex() const {
		return this->frameIndex;
	}

	//---------
	void FrameBuffer::add(const Packet & packet) {
		//store the packet
		this->packets.emplace(packet.header.packetIndex, make_unique<Packet>(packet));

		//pass any sequential packets to the decompressor
		auto firstPacketInBuffer = this->packets.begin();
		while (this->packetIndexPosition == firstPacketInBuffer->first) {
			//send it
			this->bufferToDecompressor.send(*firstPacketInBuffer->second.get());

			//remove from here
			this->packets.erase(firstPacketInBuffer);
			this->packetIndexPosition++;

			//if there's still more to do
			if (this->packets.empty()) {
				break;
			}
			else {
				firstPacketInBuffer = this->packets.begin();
			}
		}
	}

	//---------
	void FrameBuffer::clear() {
		if (!this->packets.empty()) {
			//We cleared before all our packets were processed
			auto last = max<uint16_t>(this->packetIndexPosition, this->packets.rbegin()->first);
			cout << "Dropped frame : [";
			for (int i = 0; i < last; i++) {
				auto findPacket = this->packets.find(i);
				if (findPacket == this->packets.end() && i >= this->packetIndexPosition) {
					cout << " ";
				}
				else {
					cout << ".";
				}
			}
			cout << "]" << endl;
		}
		this->message.clear();
		this->packets.clear();
		this->frameIndex = 0;
		this->packetIndexPosition = 0;

		this->stream = make_unique<ofxSquash::Stream>(this->codec, ofxSquash::Decompress);
		this->stream->setWriteFunction([this](const ofxSquash::WriteFunctionArguments & args) {
			this->writeFunction(args);
		});
	}

	//---------
	void FrameBuffer::decompressLoop() {
		while (this->threadsRunning) {
			Packet packet;
			if (this->bufferToDecompressor.receive(packet)) {
				if (!this->codec.isValid()) {
					OFXSQUASHBUDDIES_ERROR << "Codec [" << this->codec.getName() << "] is not valid. Are you sure you have the plugins installed correctly?";
					continue;
				}

				this->stream->read((const void*)packet.payload, packet.header.payloadSize);

				if (packet.header.isLastPacket)
				{
					(* this->stream) << ofxSquash::Stream::Finish();
					this->decompressorToFrameReceiver.send(this->message);
					this->clear();
				}
			}
		}
	}

	//---------
	void FrameBuffer::writeFunction(const ofxSquash::WriteFunctionArguments & args) {
		this->message.pushData(args.data, args.size);
	}

#pragma mark FrameBufferSet
	//---------
	FrameBufferSet::FrameBufferSet() {
		for (int i = 0; i < 3; i++) {
			this->frameBuffers.emplace_back(make_shared<FrameBuffer>(this->decompressorToFrameReceiver));
		}

		this->dataGramProcessorThread = thread([this]() {
			this->dataGramProcessorLoop();
		});
	}

	//---------
	FrameBufferSet::~FrameBufferSet() {
		this->threadRunning = false;
		this->socketToFrameBuffers.close();
		if (this->dataGramProcessorThread.joinable()) {
			this->dataGramProcessorThread.join();
		}
	}

	//---------
	void FrameBufferSet::setCodec(const ofxSquash::Codec & codec) {
		for (auto & frameBuffer : this->frameBuffers) {
			frameBuffer->setCodec(codec);
		}
	}

	//---------
	FrameBuffer & FrameBufferSet::getFrameBuffer(uint32_t frameIndex) {
		for (auto & frameBuffer : this->frameBuffers) {
			if (frameBuffer->getFrameIndex() == frameIndex) {
				return * frameBuffer;
			}
		}

		//if we got to here, we didn't have one matching
		auto maxDistance = 0;
		shared_ptr<FrameBuffer> furthest;
		for (auto & frameBuffer : this->frameBuffers) {
			auto distance = abs((int)frameBuffer->getFrameIndex() - (int) frameIndex);
			if (distance > maxDistance) {
				maxDistance = distance;
				furthest = frameBuffer;
			}
		}
		furthest->clear();
		furthest->setFrameIndex(frameIndex);
		return *furthest;
	}

	//---------
	bool FrameBufferSet::isExpired(uint32_t frameIndex) const {
		for (auto frameBuffer : this->frameBuffers) {
			if (frameBuffer->getFrameIndex() == frameIndex) {
				return false;
			}
		}

		auto currentMinFrameIndex = numeric_limits<uint32_t>::max();
		for (auto frameBuffer : this->frameBuffers) {
			if (frameBuffer->getFrameIndex() < currentMinFrameIndex) {
				currentMinFrameIndex = frameBuffer->getFrameIndex();
			}
		}

		if (frameIndex < currentMinFrameIndex) {
			//we're skipping backwards
			if (frameIndex + 30 < currentMinFrameIndex) {
				//ignore up to 30 before the current active frame index
				return true;
			}
		}

		return false;
	}

	//---------
	void FrameBufferSet::dataGramProcessorLoop() {
		while (this->threadRunning) {
			shared_ptr<ofxAsio::UDP::DataGram> dataGram;
			while (this->socketToFrameBuffers.receive(dataGram)) {
				auto & message = dataGram->getMessage();
				if (message.empty()) {
					continue;
				}
				Packet packet(message);

				if (this->isExpired(packet.header.frameIndex)) {
					continue;
				}

				auto & frameBuffer = this->getFrameBuffer(packet.header.frameIndex);
				frameBuffer.add(packet);
			}
		}
	}
}