#include "default_message_processor.hpp"


DefaultMessageProcessor::DefaultMessageProcessor( uint32_t worker_thread_num,
        uint32_t max_connetions)
        :_worker_thread_num(worker_thread_num)
        _max_connetions(max_connetions),
{
    //init();
}

int DefaultMessageProcessor::init()
{
    _thread_channels = new ThreadChannel[_worker_thread_num];
    _worker_threads  = new SearchThread[_worker_thread_num];
    _accessors = new ThreadChannel::Accessor[_worker_thread_num];

    //��ʼ�� �����̣߳�ÿ�������̷߳���һ��channel accessor
    for(int i=0; i < _worker_thread_num; i++)
    {
        ThreadChannelAccessor client = _thread_channels[i].connect();
        ThreadChannelAccessor server = _thread_channels[i].bind();
        _worker_threads[i].set_channel_accessor(client);
        _accessors[i]  = server;
    }
}


void DefaultMessageProcessor::start()
{
    //���������߳�
    for(int i=0; i < _worker_thread_num; i++)
    {
        _worker_threads[i].start();
    }
}

void DefaultMessageProcessor::process_input(ConnectionInfo& conn, ByteBuffer& packet)
{
    int rv = 0;

    int conn_id = conn.get_id();

    //���������


    ChannelMessge msg;
    msg.conn_id = conn_id;
    msg.buf        = bb;
    //����Ϣ���������̴߳���
    rv = _accessors[conn_id%_worker_thread_num].send(msg);
    if(rv < 0)
    {
        //
    }
}


void DefaultMessageProcessor::process_output()
{
    int rv = 0;

    ChannelMessge msg;

    //ɨ��ÿ���ܵ�,��ȡ������Ϣ
    for(int i=0; i < _worker_thread_num; i++)
    {
        // ��һ���ܵ���ȡ��Ϣ,������ȡDEFAULT_MAX_RECV_ONCE
        for(int j = 0; j < DEFAULT_MAX_RECV_ONCE; j++)
        {
            rv = _accessors[i].recv(msg);

            if( rv < 0)  //�ܵ��ѿ�,������ȡ��һ���ܵ�
            {
                break;
            }
            else        //����Ϣ���͵���Ӧ������
            {
                ConnectionMap::iterator iter = _id_connections.find(msg.conn_id);

                if(iter != _id_connections.end()) //�ҵ����ӣ�����Ϣ���ͳ�ȥ
                {
                    iter->write(*msg.buf);
                }
                else  //û���ҵ���Ӧ������
                {

                }
            }
        }
    }
}


