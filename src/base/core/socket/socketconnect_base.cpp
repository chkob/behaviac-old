/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tencent is pleased to support the open source community by making behaviac available.
//
// Copyright (C) 2015 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at http://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and limitations under the License.
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "behaviac/base/core/config.h"
#include "behaviac/base/core/socket/socketconnect_base.h"

#include "behaviac/base/core/logging/log.h"

#include "behaviac/base/core/container/hash_exmemory.h"

#include "behaviac/base/core/thread/mutex.h"

#include "behaviac/base/core/thread/wrapper.h"
#include "behaviac/base/core/container/spscqueue.h"
#include "behaviac/base/core/memory/memory.h"
#include "behaviac/base/core/memory/mempoollinked.h"

#include <cstring>	// strncpy

#if BEHAVIAC_COMPILER_MSVC
#include <windows.h>
#endif//BEHAVIAC_COMPILER_MSVC

namespace behaviac
{
    behaviac::ThreadInt			gs_threadFlag;

    static Seq				s_seq;
    Seq& GetNextSeq()
    {
        return s_seq;
    }

    // Local queue size must be power of two.
    BEHAVIAC_STATIC_ASSERT((kLocalQueueSize & (kLocalQueueSize - 1)) == 0);

#if USING_BEHAVIAC_SEQUENTIAL
    class PacketCollection
    {
    public:
        PacketCollection()
            : m_packets(0),
              m_packetsEnd(0),
              m_packetsCapacityEnd(0)
        {
        }
        ~PacketCollection()
        {
            Close();
        }

        void Init(size_t capacity)
        {
            BEHAVIAC_ASSERT(m_packets == 0);
            m_packets = BEHAVIAC_NEW_ARRAY Packet[capacity];
            m_packetsEnd = m_packets;
            m_packetsCapacityEnd = m_packets + capacity;
        }
        void Close()
        {
            if (m_packets)
            {
                BEHAVIAC_DELETE_ARRAY m_packets;
            }

            m_packets = 0;
        }

        Packet* Begin()
        {
            return m_packets;
        }
        Packet* End()
        {
            return m_packetsEnd;
        }
        size_t GetMemoryOverhead() const
        {
            return (m_packetsCapacityEnd - m_packets) * sizeof(Packet);
        }

        // False if not enough space, packet not added.
        bool Add(const Packet& packet)
        {
            if (m_packetsEnd == m_packetsCapacityEnd)
            {
                BEHAVIAC_LOG(BEHAVIAC_LOG_WARNING, "buffer overflow...\n");
                return false;
            }

            *m_packetsEnd++ = packet;
            return true;
        }

        void Reset()
        {
            m_packetsEnd = m_packets;
        }
        void Sort()
        {
            qsort(m_packets, m_packetsEnd - m_packets, sizeof(m_packets[0]), PacketCompare);
        }

    private:
        Packet*	m_packets;
        Packet*	m_packetsEnd;
        Packet*	m_packetsCapacityEnd;
    };
#else
    struct PacketCollection
    {
        void Init(size_t) {}
        void Close() {}
        size_t GetMemoryOverhead() const
        {
            return 0;
        }
    };
#endif // #if USING_BEHAVIAC_SEQUENTIAL

    class PacketBuffer
    {
    public:
        PacketBuffer(ConnectorInterface* c)
            : m_connector(c), m_free(true)
        {
        }

        void AddPacket(const Packet& packet);
#if USING_BEHAVIAC_SEQUENTIAL
        bool CollectPackets(PacketCollection& coll)
        {
            Packet* packet = m_packetQueue.Peek();

            while (packet)
            {
                if (coll.Add(*packet))
                {
                    m_packetQueue.Pop();
                    packet = m_packetQueue.Peek();

                }
                else
                {
                    return false;
                }
            }

            return true;
        }
#endif
        void SendPackets(behaviac::Socket::Handle& h)
        {
            Packet* packet = m_packetQueue.Peek();

            while (packet)
            {
                const size_t bytesToSend = packet->PrepareToSend();
                size_t bytesWritten(0);
                const bool success = behaviac::Socket::Write(h, packet, bytesToSend, bytesWritten);

                // Failed to send data. Most probably sending too much, break and
                // hope for the best next time
                if (!success || bytesWritten != bytesToSend)
                {
                    BEHAVIAC_ASSERT(0);
                    BEHAVIAC_LOG(BEHAVIAC_LOG_WARNING, "A packet is not correctly sent...\n");
                    break;
                }

                m_packetQueue.Pop();	// 'Commit' pop if data sent.
                packet = m_packetQueue.Peek();
            }
        }

        ConnectorInterface* m_connector;
        bool				m_free;

        void Clear()
        {
            Packet* packet = m_packetQueue.Peek();

            while (packet)
            {
                m_packetQueue.Pop();	// 'Commit' pop if data sent.
                packet = m_packetQueue.Peek();
            }
        }
    private:
        enum { MAX_PACKETS_IN_BUFFER = kLocalQueueSize };

        behaviac::SPSCQueue<Packet, MAX_PACKETS_IN_BUFFER>	m_packetQueue;
    };

    class CustomObjectPool : public behaviac::LinkedObjectPool<Packet>
    {
    public:
        CustomObjectPool(uint32_t objectCountPerSegment) :
            behaviac::LinkedObjectPool<Packet>(objectCountPerSegment, BEHAVIAC_OBJECTPOOL_INFINIT)
        {}

        virtual ~CustomObjectPool()
        {}
    };

#if BEHAVIAC_COMPILER_MSVC
    uint32_t t_packetBufferIndex = TLS_OUT_OF_INDEXES;
#elif BEHAVIAC_COMPILER_APPLE
    uint32_t t_packetBufferIndex = (unsigned int) - 1;
#else
    __thread uint32_t t_packetBufferIndex = (unsigned int) - 1;
#endif

    ConnectorInterface::ConnectorInterface() :
        m_port(0),
        m_writeSocket(0),
        m_packetBuffers(0),
        m_packetCollection(0),
        m_packetPool(0),
        m_maxTracedThreads(0),
        m_isInited(0),
        m_isConnected(0),
        m_isDisconnected(0),
        m_isConnectedFinished(0),
        m_terminating(0),
        m_packetsCount(0),
        s_tracerThread(0),
        m_bHandleMessage(true)
    {
    }

    ConnectorInterface::~ConnectorInterface()
    {
        //the thread has been shutdown
        this->Close();
    }

    bool ConnectorInterface::Init(int maxTracedThreads, unsigned short port, bool bBlocking)
    {
        this->Clear();
#if BEHAVIAC_COMPILER_MSVC
        t_packetBufferIndex = TlsAlloc();
        //initially 0
        //TlsSetValue(t_packetBufferIndex, 0);
#else
        //t_packetBufferIndex = 0;
        //printf("Init t_packetBufferIndex = %d\n", t_packetBufferIndex);
#endif//BEHAVIAC_COMPILER_MSVC
        m_port = (unsigned short) - 1;

        m_packetPool = BEHAVIAC_NEW CustomObjectPool(4096);
        m_packetCollection = BEHAVIAC_NEW PacketCollection;
        m_packetBuffers = BEHAVIAC_G_NEW_ARRAY(PacketBuffer*, maxTracedThreads);
        memset(m_packetBuffers, 0, sizeof(PacketBuffer*) * maxTracedThreads);
        m_maxTracedThreads = maxTracedThreads;
        m_packetCollection->Init(kGlobalQueueSize);

        if (!behaviac::Socket::InitSockets())
        {
            this->Log("behaviac: Failed to initialize sockets.\n");
            return false;
        }

        {
            BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);
            BEHAVIAC_LOG(BEHAVIAC_LOG_INFO, "behaviac: ConnectorInterface::Init Enter\n");
            BEHAVIAC_LOG1(BEHAVIAC_LOG_INFO, "behaviac: listing at port %d\n", port);
            this->ReserveThreadPacketBuffer();
            this->SetConnectPort(port);

            BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);

            {
                ScopedInt_t scopedInt(&gs_threadFlag);
                this->CreateAndStartThread();
            }

            BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);

            if (bBlocking)
            {
                BEHAVIAC_LOG(BEHAVIAC_LOG_WARNING, "behaviac: SetupConnection is blocked, please Choose 'Connect' in the Designer to continue\n");

                while (!this->IsConnected() || !this->IsConnectedFinished())
                {
                    // Wait for connection
                    behaviac::Thread::Sleep(100);
                }

                behaviac::Thread::Sleep(1);

                BEHAVIAC_ASSERT(this->IsConnected() && this->IsConnectedFinished());
            }

            BEHAVIAC_LOG(BEHAVIAC_LOG_INFO, "behaviac: ConnectorInterface::Init Connected\n");

            BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);
            //wait for the OnConnection ends
            behaviac::Thread::Sleep(200);

            BEHAVIAC_LOG(BEHAVIAC_LOG_INFO, "behaviac: ConnectorInterface::Init successful\n");

            BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);
        }

        AtomicInc(m_isInited);

        return m_packetBuffers != 0;
    }

    void ConnectorInterface::Close()
    {
        AtomicInc(m_terminating);
        AtomicDec(m_isConnectedFinished);

        AtomicInc(m_isDisconnected);

        if (s_tracerThread)
        {
            if (!thread::IsThreadTerminated(s_tracerThread))
            {
                while (IsConnected() && !thread::IsThreadTerminated(s_tracerThread))
                {
                    behaviac::Thread::Sleep(1);
                }
            }

            {
                ScopedLock lock(m_packetBuffersLock);

                for (int i = 0; i < this->m_maxTracedThreads; ++i)
                {
                    BEHAVIAC_DELETE(m_packetBuffers[i]);
                }

                BEHAVIAC_G_DELETE_ARRAY(m_packetBuffers);
                m_packetBuffers = 0;
            }

            if (!thread::IsThreadTerminated(s_tracerThread))
            {
                thread::StopThread(s_tracerThread);
            }

            s_tracerThread = 0;
        }

        if (m_packetCollection)
        {
            m_packetCollection->Close();
            BEHAVIAC_DELETE(m_packetCollection);
            m_packetCollection = 0;
        }

        BEHAVIAC_DELETE(m_packetPool);
        m_packetPool = 0;
#if BEHAVIAC_COMPILER_MSVC

        if (t_packetBufferIndex != TLS_OUT_OF_INDEXES)
        {
            TlsFree(t_packetBufferIndex);
            t_packetBufferIndex = TLS_OUT_OF_INDEXES;
        }

#else
        t_packetBufferIndex = 0;
#endif
        behaviac::Socket::ShutdownSockets();

        AtomicDec(m_isInited);
    }

    static void MemTracer_ThreadFunc(ConnectorInterface* tracer)
    {
        tracer->ThreadFunc();
    }

    void ConnectorInterface::CreateAndStartThread()
    {
        thread::ThreadHandle threadHandle = behaviac::thread::CreateAndStartThread((behaviac::thread::ThreadFunction*)&MemTracer_ThreadFunc, this, 16 * 1024);

        s_tracerThread = threadHandle;
    }

    bool ConnectorInterface::IsConnected() const
    {
        return m_isConnected != 0 && this->m_writeSocket;
    }

    bool ConnectorInterface::IsDisconnected() const
    {
        return m_isDisconnected != 0;
    }

    bool ConnectorInterface::IsConnectedFinished() const
    {
        return m_isConnectedFinished != 0;
    }

    bool ConnectorInterface::IsInited() const
    {
        return m_isInited != 0;
    }

    void ConnectorInterface::SetConnectPort(unsigned short port)
    {
        this->m_port = port;
    }

    int ConnectorInterface::GetBufferIndex(bool bReserve)
    {
#if BEHAVIAC_COMPILER_MSVC
        BEHAVIAC_ASSERT(t_packetBufferIndex != TLS_OUT_OF_INDEXES);
        int bufferIndex = (int)TlsGetValue(t_packetBufferIndex);
#else
        BEHAVIAC_ASSERT(t_packetBufferIndex != (unsigned int) - 1);
        int bufferIndex = (int)t_packetBufferIndex;
#endif
        //WHEN bReserve is false, it is unsafe to allocate memory as other threads might be allocating
        //you can avoid the following assert to malloc a block of memory in your thread at the very beginning
        BEHAVIAC_ASSERT(bufferIndex > 0 || bReserve);

        //bufferIndex initially is 0
        if (bufferIndex <= 0 && bReserve)
        {
            bufferIndex = ReserveThreadPacketBuffer();
        }

        return bufferIndex;
    }


    void ConnectorInterface::AddPacket(const Packet& packet, bool bReserve)
    {
        if (this->IsConnected() && this->m_writeSocket != 0)
        {
            int bufferIndex = this->GetBufferIndex(bReserve);

            if (bufferIndex > 0)
            {
                m_packetBuffers[bufferIndex]->AddPacket(packet);

                this->m_packetsCount++;
            }
        }
    }

    void ConnectorInterface::RecordText(const char* text)
    {
        if (this->m_packetPool)
        {
            //if it is out of memory here, please check 'SetupConnection'
            Packet* pP = this->m_packetPool->Allocate();

            if (pP)
            {
                pP->Init(CommandId::CMDID_TEXT, s_seq.Next());

                Text* pT = (Text*)pP->data;
                strncpy(pT->buffer, text, kMaxTextLength);
                pT->buffer[kMaxTextLength] = '\0';
            }
        }
    }

    void ConnectorInterface::SendAllPackets()
    {
        for (int i = 0; i < m_maxTracedThreads; ++i)
        {
            if (m_packetBuffers[i] && !m_packetBuffers[i]->m_free)
            {
#if USING_BEHAVIAC_SEQUENTIAL

                if (!m_packetBuffers[i]->CollectPackets(*m_packetCollection))
                {
                    break;
                }

#else
                m_packetBuffers[i].SendPackets(m_writeSocket);
#endif
            }
        }

#if USING_BEHAVIAC_SEQUENTIAL
        // TODO: Deal with Socket::Write failures.
        // (right now packet is lost).
        m_packetCollection->Sort();

        for (Packet* p = m_packetCollection->Begin(); p != m_packetCollection->End() && this->m_writeSocket; ++p)
        {
            const size_t bytesToSend = p->PrepareToSend();
            size_t bytesWritten(0);
            behaviac::Socket::Write(m_writeSocket, p, bytesToSend, bytesWritten);
        }

        m_packetCollection->Reset();
#endif
        this->m_packetsCount = 0;
    }

    int ConnectorInterface::ReserveThreadPacketBuffer()
    {
#if BEHAVIAC_COMPILER_MSVC
        int bufferIndex = (int)TlsGetValue(t_packetBufferIndex);
#else
        int bufferIndex = t_packetBufferIndex;
#endif
        //THREAD_ID_TYPE id = behaviac::GetTID();
        //BEHAVIAC_LOGINFO("ReserveThreadPacketBuffer:%d thread %d\n", bufferIndex, id);

        //bufferIndex initially is 0
        if (bufferIndex <= 0)
        {
            int retIndex(-2);

            ScopedLock lock(m_packetBuffersLock);

            // NOTE: This is quite naive attempt to make sure that main thread queue
            // is the last one (rely on the fact that it's most likely to be the first
            // one trying to send message). This means EndFrame event should be sent after
            // memory operations from that frame.
            // (doesn't matter in SEQUENTIAL mode).
            for (int i = m_maxTracedThreads - 1; i >= 0; --i)
            {
                if (!m_packetBuffers[i])
                {
                    ScopedInt_t scopedInt(&gs_threadFlag);
                    m_packetBuffers[i] = BEHAVIAC_NEW PacketBuffer(this);
                }

                if (m_packetBuffers[i])
                {
                    if (m_packetBuffers[i]->m_free)
                    {
                        m_packetBuffers[i]->m_free = false;
                        retIndex = i;
                        break;
                    }
                }
            }

            if (retIndex > 0)
            {
#if BEHAVIAC_COMPILER_MSVC
                TlsSetValue(t_packetBufferIndex, (PVOID)retIndex);
#else
                t_packetBufferIndex = retIndex;
#endif

            }
            else
            {
                Log("behaviac: Couldn't reserve packet buffer, too many active threads.\n");
                BEHAVIAC_ASSERT(false);
            }

            bufferIndex = retIndex;

            //BEHAVIAC_LOGINFO("ReserveThreadPacketBuffer:%d thread %d\n", bufferIndex, id);
        }

        return bufferIndex;
    }

    bool ConnectorInterface::ReceivePackets(const char* msgCheck)
    {
        const int kBufferLen = 2048;
        char buffer[kBufferLen];

        bool found = false;

        while (size_t reads = behaviac::Socket::Read(m_writeSocket, buffer, kBufferLen))
        {
            buffer[reads] = '\0';
            //BEHAVIAC_LOG(MEMDIC_LOG_INFO, buffer);
            //printf("ReceivePackets %s\n", buffer);

            {
                ScopedLock lock(ms_cs);

                ms_texts += buffer;
            }

            if (msgCheck && strstr(buffer, msgCheck))
            {
                //printf("ReceivePackets found\n");
                found = true;
            }

            if (this->m_writeSocket == 0)
            {
                break;
            }
        }

        if (this->m_bHandleMessage)
        {
            behaviac::string msgs;

            if (this->ReadText(msgs))
            {
                this->OnRecieveMessages(msgs);

                return true;
            }
        }

        return found;
    }

    void ConnectorInterface::OnRecieveMessages(const behaviac::string& msgs)
    {
        BEHAVIAC_UNUSED_VAR(msgs);
    }

    void ConnectorInterface::ThreadFunc()
    {
#if BEHAVIAC_COMPILER_MSVC
        //printf("ThreadFunc gs_threadFlag = %d\n", (int)gs_threadFlag.value());
        BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);
#endif
        {
            ScopedInt_t scopedInt(&gs_threadFlag);
            Log("behaviac: Socket Thread Starting\n");
#if BEHAVIAC_COMPILER_MSVC
            BEHAVIAC_ASSERT(t_packetBufferIndex != TLS_OUT_OF_INDEXES);
#else
            //printf("ThreadFunc t_packetBufferIndex = %d\n", t_packetBufferIndex);
            //BEHAVIAC_ASSERT(t_packetBufferIndex != (unsigned int)-1);
#endif//
        }
        namespace Socket = behaviac::Socket;
        const bool blockingSocket = true;
        behaviac::Socket::Handle	serverSocket = 0;
        {
            ScopedInt_t scopedInt(&gs_threadFlag);
            serverSocket = Socket::Create(blockingSocket);

            if (!serverSocket)
            {
                Log("behaviac: Couldn't create server socket.\n");
                return;
            }

            char bufferTemp[64];
            string_sprintf(bufferTemp, "behaviac: Listening at port %d...\n", m_port);
            Log(bufferTemp);

            // max connections: 1, don't allow multiple clients?
            if (!Socket::Listen(serverSocket, m_port, 1))
            {
                Log("behaviac: Couldn't configure server socket.\n");
                Socket::Close(serverSocket);
                return;
            }
        }
#if BEHAVIAC_COMPILER_MSVC
        BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);
#endif

        this->ReserveThreadPacketBuffer();

        while (!m_terminating)
        {
#if BEHAVIAC_COMPILER_MSVC

            //wait for connecting
            while (!m_terminating)
            {
                //Log("Socket::TestConnection.\n");
                if (Socket::TestConnection(serverSocket))
                {
                    break;
                }

                behaviac::Thread::Sleep(100);
            }

#endif

            if (!m_terminating)
            {
                BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);
                Log("behaviac: accepting...\n");
                {
                    ScopedInt_t scopedInt(&gs_threadFlag);
                    m_writeSocket = Socket::Accept(serverSocket, kSocketBufferSize);

                    if (!m_writeSocket)
                    {
                        Log("behaviac: Couldn't create write socket.\n");
                        Socket::Close(serverSocket);
                        return;
                    }

                    Log("behaviac: connection accepted\n");
                }

                BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);

                {
                    ScopedInt_t scopedInt(&gs_threadFlag);

                    AtomicInc(m_isConnected);
                    behaviac::Thread::Sleep(1);

                    OnConnection();

                    AtomicInc(m_isConnectedFinished);
                    behaviac::Thread::Sleep(1);

                    //this->OnConnectionFinished();

                    Log("behaviac: after Connected.\n");
                }

                BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);

                while (!m_terminating && this->m_writeSocket)
                {
                    behaviac::Thread::Sleep(1);
                    SendAllPackets();

                    ReceivePackets();
                }

                BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);

                // One last time, to send any outstanding packets out there.
                if (this->m_writeSocket)
                {
                    SendAllPackets();
                }

                Socket::Close(m_writeSocket);
                this->Clear();

                Log("behaviac: disconnected. \n");
            }
        }//while (!m_terminating)

        Socket::Close(serverSocket);

        this->Clear();

        BEHAVIAC_ASSERT(gs_threadFlag.value() == 0);

        Log("behaviac: ThreadFunc exited. \n");
    }

    size_t ConnectorInterface::GetMemoryOverhead() const
    {
        size_t threads = GetNumTrackedThreads();
        size_t bufferSize = sizeof(PacketBuffer) * threads;
        size_t packetCollectionSize = m_packetCollection ? m_packetCollection->GetMemoryOverhead() : 0;
        size_t packetPoolSize = m_packetPool ? m_packetPool->GetMemoryUsage() : 0;
        return bufferSize + packetCollectionSize + packetPoolSize;
    }

    size_t ConnectorInterface::GetNumTrackedThreads() const
    {
        size_t numTrackedThreads(0);

        if (m_packetBuffers)
        {
            for (int i = 0; i < m_maxTracedThreads; ++i)
            {
                if (m_packetBuffers[i] && !m_packetBuffers[i]->m_free)
                {
                    ++numTrackedThreads;
                }
            }
        }

        return numTrackedThreads;
    }

    int ConnectorInterface::GetPacketsCount() const
    {
        //not thread safe
        return m_packetsCount;
    }

    void ConnectorInterface::Log(const char* msg)
    {
        ScopedInt_t scopedInt(&gs_threadFlag);

        BEHAVIAC_LOGMSG(msg);
    }

    void ConnectorInterface::Clear()
    {
        this->m_isConnected = 0;
        this->m_isDisconnected = 0;
        this->m_isConnectedFinished = 0;
        this->m_terminating = 0;

        if (this->m_packetBuffers)
        {
            int bufferIndex = this->GetBufferIndex(false);

            if (bufferIndex > 0)
            {
                this->m_packetBuffers[bufferIndex]->Clear();
            }
        }

        if (this->m_packetPool)
        {
            behaviac::LinkedObjectPool<Packet>::Iterator it = this->m_packetPool->Begin();

            while (!it.Empty())
            {
                Packet* p = (*it);
                ++it;
                this->m_packetPool->Free(p);
            }
        }

        if (this->m_packetCollection)
        {
            this->m_packetCollection->Reset();
        }

        this->m_packetsCount = 0;
    }

    void ConnectorInterface::SendExistingPackets()
    {
        behaviac::LinkedObjectPool<Packet>::Iterator it = this->m_packetPool->Begin();

        int packetsCount = 0;

        while (!it.Empty())
        {
            Packet* p = (*it);
            const size_t bytesToSend = p->PrepareToSend();
            size_t bytesWritten(0);
            behaviac::Socket::Write(m_writeSocket, p, bytesToSend, bytesWritten);
            packetsCount++;
            //this->m_packetPool->Free(p);
            ++it;
        }

        //wait for the finish
        behaviac::Thread::Sleep(1000);
        int packetsCount2 = 0;

        it = this->m_packetPool->Begin();

        while (!it.Empty())
        {
            Packet* p = (*it);
            ++it;
            packetsCount2++;
            this->m_packetPool->Free(p);
        }

        //this->m_packetPool->Destroy(true);
    }

    void ConnectorInterface::SendText(const char* text, uint8_t commandId)
    {
        if (this->IsConnected())
        {
            Packet packet(commandId, s_seq.Next());

            Text* pT = (Text*)packet.data;
            strncpy(pT->buffer, text, kMaxTextLength);
            pT->buffer[kMaxTextLength] = '\0';
            this->AddPacket(packet, true);
            gs_packetsStats.texts++;
        }
    }

    bool ConnectorInterface::ReadText(behaviac::string& text)
    {
        if (this->IsConnected())
        {
            ScopedLock lock(ms_cs);

            text = this->ms_texts;
            this->ms_texts.clear();

            return !text.empty();
        }

        return false;
    }

    void PacketBuffer::AddPacket(const Packet& packet)
    {
        // Spin loop until there is a place for new packet.
        // If this happens to often, it means we are producing packets
        // quicker than consuming them, increasing max # of packets in buffer
        // could help.
        while (m_packetQueue.IsFull())
        {
            //BEHAVIAC_LOGINFO("packet buffer is full... buffer size: %d\n", MAX_PACKETS_IN_BUFFER);
            behaviac::Thread::Sleep(1);

            if (!this->m_connector->GetWriteSocket())
            {
                break;
            }
        }

        m_packetQueue.Push(packet);
    }
}
