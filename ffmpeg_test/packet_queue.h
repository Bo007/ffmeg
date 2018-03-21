#pragma once

extern "C"
{
#include <libavformat\avformat.h>
}
#include <mutex>
#include <condition_variable>

class PacketQueue
{
	AVPacketList *m_firstPacket;
	AVPacketList *m_lastPacket;
	
	int m_packetsCount;
	int m_size;

	std::mutex *m_mutex;
	std::condition_variable *m_cond;
public:

	static AVPacket m_flush_pkt;
	static bool m_state;

	int size() { return m_size; }
	void notifyOne() { m_cond->notify_one(); }

	int packet_queue_put(AVPacket *pkt);
	int packet_queue_get(AVPacket *pkt, int block);
	void packet_queue_flush();

	PacketQueue();

	~PacketQueue();
};

