#include "Packet_Queue.h"

AVPacket PacketQueue::m_flush_pkt;
bool PacketQueue::m_state;

int PacketQueue::packet_queue_put(AVPacket * pkt)
{
	AVPacketList *pkt1;
	if (pkt != &m_flush_pkt && av_dup_packet(pkt) < 0)
		return -1;
	pkt1 = static_cast<AVPacketList*>(av_malloc(sizeof(AVPacketList)));
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = NULL;

	m_mutex->lock();

	if (!m_lastPacket)
		m_firstPacket = pkt1;
	else
		m_lastPacket->next = pkt1;
	m_lastPacket = pkt1;
	m_packetsCount++;
	m_size += pkt1->pkt.size;
	m_cond->notify_one();

	m_mutex->unlock();
	return 0;
}

int PacketQueue::packet_queue_get(AVPacket * pkt, int block)
{
	AVPacketList *pkt1;
	int ret;

	m_mutex->lock();

	for (;;)
	{
		if (m_state)
		{
			ret = -1;
			break;
		}

		pkt1 = m_firstPacket;
		if (pkt1)
		{
			m_firstPacket = pkt1->next;
			if (!m_firstPacket)
				m_lastPacket = NULL;
			m_packetsCount--;
			m_size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		}
		else if (!block)
		{
			ret = 0;
			break;
		}
		else
			m_cond->wait(std::unique_lock<std::mutex>(*m_mutex, std::defer_lock));
	}
	m_mutex->unlock();
	return ret;
}

void PacketQueue::packet_queue_flush()
{
	AVPacketList *pkt, *pkt1;

	m_mutex->lock();
	for (pkt = m_firstPacket; pkt != NULL; pkt = pkt1)
	{
		pkt1 = pkt->next;
		av_free_packet(&pkt->pkt);
		av_freep(&pkt);
	}
	m_lastPacket = NULL;
	m_firstPacket = NULL;
	m_packetsCount = 0;
	m_size = 0;
	m_mutex->unlock();
}

PacketQueue::PacketQueue()
{
	m_firstPacket = NULL;
	m_lastPacket = NULL;

	m_packetsCount = 0;
	m_size = 0;

	m_mutex = new std::mutex();
	m_cond = new std::condition_variable();

	m_state = false;
}

PacketQueue::~PacketQueue()
{
	m_mutex->lock();
	delete m_mutex;
	delete m_cond;
}
